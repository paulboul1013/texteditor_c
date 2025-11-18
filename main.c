#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

struct termios orig_termios;

#define VISIBLE_LINES 15  // 一次顯示的行數

// 剪貼板緩衝區（用於複製貼上）
char clipboard[512] = {0};
int clipboard_has_content = 0;  // 標記剪貼板是否有內容

// 編輯器狀態結構體（每個文件一個）
typedef struct {
    int type;               // 逆操作類型
    int line;               // 相關行號
    char content[512];      // 逆操作所需內容（例如原行內容或插入內容）
} UndoEntry;

typedef struct {
    char filename[256];
    char buffer[1024];
    int current_line;
    int row_offset;
    int total_lines;
    char search_term[128];
    int search_mode;
    int search_result_line;
    int search_result_offset;
    int total_matches;
    int current_match;
    UndoEntry undo_stack[100];
    int undo_top;
    int suppress_undo;  // 正在執行復原時避免將操作再次推入堆疊
} EditorState;

// 全局變數
EditorState editors[2];  // 最多兩個編輯器
int num_editors = 0;     // 實際編輯器數量（1或2）
int active_editor = 0;   // 當前活動的編輯器（0或1）

// 函式前置宣告
int count_lines(char *buffer);
void save_editor(EditorState *ed);
void insert_new_line(EditorState *ed, int after_line);
int delete_line(EditorState *ed, int line_to_delete);
void paste_line(EditorState *ed, int after_line);
static void undo_last_action(EditorState *ed);
// 供 undo 使用之前置宣告，避免隱式宣告
static void replace_line_silent(char *buffer, int line_no, const char *new_content);
static void insert_after_silent(char *buffer, int after_line, const char *payload);
static void delete_line_silent(char *buffer, int line_to_delete);
char read_key();

// ===== Live Share（即時共同編輯）相關 =====
enum {
	LIVE_NONE = 0,
	LIVE_HOST = 1,
	LIVE_JOIN = 2
};

enum LiveOpType {
	OP_SYNC_FULL = 1,
	OP_EDIT_LINE = 2,
	OP_INSERT_AFTER = 3,
	OP_DELETE_LINE = 4,
	OP_PASTE_AFTER = 5,
	OP_CURSOR = 6,
	OP_HELLO = 7
};

// Undo 逆操作類型
enum UndoOpType {
	UNDO_NONE = 0,
	UNDO_SET_LINE = 1,                    // 將指定行設定為 content
	UNDO_DELETE_LINE = 2,                 // 刪除指定行
	UNDO_INSERT_AFTER_WITH_CONTENT = 3    // 在 line 之後插入 content
};

static int live_mode = LIVE_NONE;       // 0: 關閉, 1: 主機, 2: 加入
static int live_server_sock = -1;       // 僅主機使用，用於 listen
static int live_sock = -1;              // 已連線的對等端
static volatile int live_running = 0;   // 收發執行緒運行旗標
static pthread_t live_thread;
static pthread_mutex_t editor_mutex[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };
static volatile int live_remote_line = 0; // 已廢棄（保留避免破壞原行為）

#define MAX_PEERS 20
static int live_self_id = 1; // host 為 1；client 由主機指定
static int live_peer_line[MAX_PEERS + 1] = {0}; // 1..MAX_PEERS 的每位參與者所在行
static int live_peer_col[MAX_PEERS + 1] = {0};  // 1..MAX_PEERS 的每位參與者所在欄位（內容游標）

// Host 端多連線管理
typedef struct {
	int fd;
	int id;
	int in_use;
	pthread_t thread;
} ClientInfo;

static ClientInfo live_clients[MAX_PEERS] = {0};
static pthread_mutex_t live_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static int next_assign_id = 2;

static void live_lock_editor(int idx) {
	if (idx >= 0 && idx < 2) {
		pthread_mutex_lock(&editor_mutex[idx]);
	}
}
static void live_unlock_editor(int idx) {
	if (idx >= 0 && idx < 2) {
		pthread_mutex_unlock(&editor_mutex[idx]);
	}
}

static int send_all(int sock, const void *buf, size_t len) {
	const char *p = (const char *)buf;
	size_t left = len;
	while (left > 0) {
		ssize_t n = send(sock, p, left, 0);
		if (n <= 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += n;
		left -= (size_t)n;
	}
	return 0;
}

static int recv_line(int sock, char *buf, size_t max) {
	size_t i = 0;
	while (i + 1 < max) {
		char c;
		ssize_t n = recv(sock, &c, 1, 0);
		if (n <= 0) return -1;
		buf[i++] = c;
		if (c == '\n') break;
	}
	buf[i] = '\0';
	return (int)i;
}

static int recv_all(int sock, void *buf, size_t len) {
	char *p = (char *)buf;
	size_t left = len;
	while (left > 0) {
		ssize_t n = recv(sock, p, left, 0);
		if (n <= 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += n;
		left -= (size_t)n;
	}
	return 0;
}

static void send_header_payload_to_fd(int fd, const char *header, size_t hlen, const char *payload, size_t plen) {
	if (fd < 0) return;
	if (send_all(fd, header, hlen) != 0) return;
	if (plen > 0 && payload) {
		send_all(fd, payload, plen);
	}
}

static void broadcast_header_payload_except(int except_fd, const char *header, size_t hlen, const char *payload, size_t plen) {
	pthread_mutex_lock(&live_clients_mutex);
	for (int i = 0; i < MAX_PEERS; i++) {
		if (live_clients[i].in_use && live_clients[i].fd >= 0 && live_clients[i].fd != except_fd) {
			send_header_payload_to_fd(live_clients[i].fd, header, hlen, payload, plen);
		}
	}
	pthread_mutex_unlock(&live_clients_mutex);
}

static void live_broadcast_simple(enum LiveOpType t, int line) {
	char header[128];
	int header_len = snprintf(header, sizeof(header), "OP %d %d 0\n", (int)t, line);
	if (header_len <= 0) return;
	if (live_mode == LIVE_HOST) {
		broadcast_header_payload_except(-1, header, (size_t)header_len, NULL, 0);
	} else if (live_mode == LIVE_JOIN) {
		if (live_sock >= 0) {
			send_all(live_sock, header, (size_t)header_len);
		}
	}
}

static void live_broadcast_with_payload(enum LiveOpType t, int line, const char *payload) {
	size_t plen = payload ? strlen(payload) : 0;
	char header[128];
	int header_len = snprintf(header, sizeof(header), "OP %d %d %zu\n", (int)t, line, plen);
	if (header_len <= 0) return;
	if (live_mode == LIVE_HOST) {
		broadcast_header_payload_except(-1, header, (size_t)header_len, payload, plen);
	} else if (live_mode == LIVE_JOIN) {
		if (live_sock >= 0) {
			if (send_all(live_sock, header, (size_t)header_len) != 0) return;
			if (plen > 0) send_all(live_sock, payload, plen);
		}
	}
}

static void live_broadcast_cursor(int current_line, int current_col) {
	// 格式："id line col"
	char buf[64];
	int n = snprintf(buf, sizeof(buf), "%d %d %d", live_self_id, current_line, current_col);
	if (n <= 0) return;
	live_broadcast_with_payload(OP_CURSOR, 0, buf);
}

static void editor_recount_and_clamp(EditorState *ed) {
	ed->total_lines = count_lines(ed->buffer);
	if (ed->total_lines < 1) ed->total_lines = 1;
	if (ed->current_line < 1) ed->current_line = 1;
	if (ed->current_line > ed->total_lines) ed->current_line = ed->total_lines;
	if (ed->row_offset < 1) ed->row_offset = 1;
	if (ed->current_line >= ed->row_offset + VISIBLE_LINES) {
		ed->row_offset = ed->current_line - VISIBLE_LINES + 1;
	}
	if (ed->current_line < ed->row_offset) {
		ed->row_offset = ed->current_line;
	}
}

// ===== Undo 工具 =====
static void push_undo(EditorState *ed, int type, int line, const char *content) {
    if (!ed || ed->suppress_undo) return;
    if (ed->undo_top >= (int)(sizeof(ed->undo_stack) / sizeof(ed->undo_stack[0]))) {
        // 滿了就移除最舊的一個
        for (int i = 1; i < ed->undo_top; i++) {
            ed->undo_stack[i - 1] = ed->undo_stack[i];
        }
        ed->undo_top--;
    }
    UndoEntry *e = &ed->undo_stack[ed->undo_top++];
    e->type = type;
    e->line = line;
    if (content) {
        strncpy(e->content, content, sizeof(e->content) - 1);
        e->content[sizeof(e->content) - 1] = '\0';
    } else {
        e->content[0] = '\0';
    }
}

static void undo_last_action(EditorState *ed) {
    if (!ed) return;
    if (ed->undo_top <= 0) {
        printf("\n✗ 沒有可復原的動作\n");
        printf("按任意鍵繼續...");
        read_key();
        return;
    }
    UndoEntry entry = ed->undo_stack[--ed->undo_top];
    int ed_idx = (ed == &editors[0]) ? 0 : 1;
    ed->suppress_undo = 1;
    live_lock_editor(ed_idx);
    if (entry.type == UNDO_SET_LINE) {
        replace_line_silent(ed->buffer, entry.line, entry.content);
        live_unlock_editor(ed_idx);
        editor_recount_and_clamp(ed);
        ed->current_line = entry.line;
        live_broadcast_with_payload(OP_EDIT_LINE, entry.line, entry.content);
    } else if (entry.type == UNDO_DELETE_LINE) {
        delete_line_silent(ed->buffer, entry.line);
        live_unlock_editor(ed_idx);
        editor_recount_and_clamp(ed);
        if (ed->current_line > ed->total_lines) ed->current_line = ed->total_lines;
        if (ed->current_line < 1) ed->current_line = 1;
        live_broadcast_simple(OP_DELETE_LINE, entry.line);
    } else if (entry.type == UNDO_INSERT_AFTER_WITH_CONTENT) {
        insert_after_silent(ed->buffer, entry.line, entry.content);
        live_unlock_editor(ed_idx);
        editor_recount_and_clamp(ed);
        ed->current_line = entry.line + 1;
        live_broadcast_with_payload(OP_PASTE_AFTER, entry.line, entry.content);
    } else {
        live_unlock_editor(ed_idx);
    }
    ed->suppress_undo = 0;
    // 自動保存與訊息
    save_editor(ed);
    // printf("\n✓ 已復原上一個動作\n");
    // printf("按任意鍵繼續...");
    // read_key();
    // 廣播目前游標（非編輯模式，欄位用 0）
    live_broadcast_cursor(ed->current_line, 0);
}

// 在指定行替換為新內容（不包含換行），保留行後剩餘內容
static void replace_line_silent(char *buffer, int line_no, const char *new_content) {
	if (line_no < 1) return;
	char *line_start = buffer;
	for (int i = 0; i < line_no - 1; i++) {
		char *next = strchr(line_start, '\n');
		if (!next) {
			// 補到最後為止
			line_start = buffer + strlen(buffer);
			break;
		}
		line_start = next + 1;
	}
	char *line_end = strchr(line_start, '\n');
	char after_line[512] = {0};
	if (line_end) {
		strcpy(after_line, line_end);
	} else {
		after_line[0] = '\0';
	}
	size_t new_len = new_content ? strlen(new_content) : 0;
	// 將新內容寫入並接回後續
	if (new_len > 0) {
		strcpy(line_start, new_content);
		strcpy(line_start + new_len, after_line);
	} else {
		// 清空此行
		strcpy(line_start, after_line[0] ? after_line + 1 : "");
	}
}

// 在 after_line 之後插入一行，內容為 payload（可為空）
static void insert_after_silent(char *buffer, int after_line, const char *payload) {
	char *insert_pos = buffer;
	if (after_line > 0) {
		for (int i = 0; i < after_line; i++) {
			char *next = strchr(insert_pos, '\n');
			if (next) {
				insert_pos = next + 1;
			} else {
				insert_pos = buffer + strlen(buffer);
				break;
			}
		}
	}
	char after_content[512] = {0};
	strcpy(after_content, insert_pos);
	const char *content = payload ? payload : "";
	strcpy(insert_pos, content);
	strcpy(insert_pos + strlen(content), "\n");
	strcpy(insert_pos + strlen(content) + 1, after_content);
}

// 刪除此行（不做任何 UI 提示）
static void delete_line_silent(char *buffer, int line_to_delete) {
	int total = count_lines(buffer);
	if (total <= 1) {
		return;
	}
	char *line_start = buffer;
	for (int i = 0; i < line_to_delete - 1; i++) {
		char *next = strchr(line_start, '\n');
		if (next) {
			line_start = next + 1;
		} else {
			return;
		}
	}
	char *line_end = strchr(line_start, '\n');
	char after_content[512] = {0};
	if (line_end) {
		strcpy(after_content, line_end + 1);
		strcpy(line_start, after_content);
	} else {
		if (line_start > buffer && *(line_start - 1) == '\n') {
			*(line_start - 1) = '\0';
		} else {
			*line_start = '\0';
		}
	}
}

static void apply_remote_op(enum LiveOpType t, int line, const char *payload, size_t plen) {
	// 目前僅同步第一個編輯器
	EditorState *ed = &editors[0];
	live_lock_editor(0);
	if (t == OP_SYNC_FULL) {
		size_t copy_len = (plen < sizeof(ed->buffer) - 1) ? plen : sizeof(ed->buffer) - 1;
		memset(ed->buffer, 0, sizeof(ed->buffer));
		if (copy_len > 0) {
			memcpy(ed->buffer, payload, copy_len);
			ed->buffer[copy_len] = '\0';
		}
		editor_recount_and_clamp(ed);
	} else if (t == OP_EDIT_LINE) {
		char tmp[512];
		size_t copy_len = (plen < sizeof(tmp) - 1) ? plen : sizeof(tmp) - 1;
		memcpy(tmp, payload, copy_len);
		tmp[copy_len] = '\0';
		replace_line_silent(ed->buffer, line, tmp);
		editor_recount_and_clamp(ed);
	} else if (t == OP_INSERT_AFTER) {
		char tmp[512] = {0};
		if (plen > 0) {
			size_t copy_len = (plen < sizeof(tmp) - 1) ? plen : sizeof(tmp) - 1;
			memcpy(tmp, payload, copy_len);
			tmp[copy_len] = '\0';
		}
		insert_after_silent(ed->buffer, line, tmp);
		editor_recount_and_clamp(ed);
	} else if (t == OP_DELETE_LINE) {
		delete_line_silent(ed->buffer, line);
		editor_recount_and_clamp(ed);
	} else if (t == OP_PASTE_AFTER) {
		char tmp[512] = {0};
		size_t copy_len = (plen < sizeof(tmp) - 1) ? plen : sizeof(tmp) - 1;
		memcpy(tmp, payload, copy_len);
		tmp[copy_len] = '\0';
		insert_after_silent(ed->buffer, line, tmp);
		editor_recount_and_clamp(ed);
	} else if (t == OP_CURSOR) {
		// payload: "id line col"
		int pid = 0, pline = 0, pcol = 0;
		if (payload && plen > 0) {
			char tmp[64] = {0};
			size_t copy_len = (plen < sizeof(tmp) - 1) ? plen : sizeof(tmp) - 1;
			memcpy(tmp, payload, copy_len);
			tmp[copy_len] = '\0';
			sscanf(tmp, "%d %d %d", &pid, &pline, &pcol);
			if (pid >= 1 && pid <= MAX_PEERS && pid != live_self_id) {
				live_peer_line[pid] = pline;
				if (pcol < 0) pcol = 0;
				live_peer_col[pid] = pcol;
			}
		}
	} else if (t == OP_HELLO) {
		// 僅 client 端接收：設定 self id
		if (payload && plen > 0 && live_mode == LIVE_JOIN) {
			char tmp[32] = {0};
			size_t copy_len = (plen < sizeof(tmp) - 1) ? plen : sizeof(tmp) - 1;
			memcpy(tmp, payload, copy_len);
			tmp[copy_len] = '\0';
			int assigned = atoi(tmp);
			if (assigned >= 1 && assigned <= MAX_PEERS) {
				live_self_id = assigned;
			}
		}
	}
	live_unlock_editor(0);
}

// ===== Host 端：每個客戶端的接收線程 =====
static void *host_client_thread(void *arg) {
	int idx = *(int*)arg;
	free(arg);
	int cfd = -1;
	int cid = 0;
	{
		pthread_mutex_lock(&live_clients_mutex);
		if (idx >= 0 && idx < MAX_PEERS && live_clients[idx].in_use) {
			cfd = live_clients[idx].fd;
			cid = live_clients[idx].id;
		}
		pthread_mutex_unlock(&live_clients_mutex);
	}
	if (cfd < 0 || cid <= 0) return NULL;

	// 發送 HELLO 與完整同步、目前已知的游標位置
	{
		char idbuf[32];
		int idlen = snprintf(idbuf, sizeof(idbuf), "%d", cid);
		char header[128];
		int header_len = snprintf(header, sizeof(header), "OP %d 0 %d\n", (int)OP_HELLO, idlen);
		send_header_payload_to_fd(cfd, header, (size_t)header_len, idbuf, (size_t)idlen);

		// 發送完整內容
		EditorState *ed = &editors[0];
		size_t plen = strlen(ed->buffer);
		header_len = snprintf(header, sizeof(header), "OP %d 0 %zu\n", (int)OP_SYNC_FULL, plen);
		send_header_payload_to_fd(cfd, header, (size_t)header_len, ed->buffer, plen);

		// 發送當前已知游標（包含主機自己與其他人）
		for (int i = 1; i <= MAX_PEERS; i++) {
			if (live_peer_line[i] > 0) {
				char payload[64];
				int n = snprintf(payload, sizeof(payload), "%d %d %d", i, live_peer_line[i], live_peer_col[i]);
				header_len = snprintf(header, sizeof(header), "OP %d 0 %d\n", (int)OP_CURSOR, n);
				send_header_payload_to_fd(cfd, header, (size_t)header_len, payload, (size_t)n);
			}
		}
	}

	// 接收循環
	while (live_running) {
		char header[128];
		if (recv_line(cfd, header, sizeof(header)) <= 0) {
			break;
		}
		int t = 0, line = 0;
		size_t len = 0;
		if (sscanf(header, "OP %d %d %zu", &t, &line, &len) != 3) {
			continue;
		}
		char *payload = NULL;
		if (len > 0) {
			payload = (char *)malloc(len);
			if (!payload) break;
			if (recv_all(cfd, payload, len) != 0) {
				free(payload);
				break;
			}
		}
		// 先轉發給其它客戶端（不含來源）
		broadcast_header_payload_except(cfd, header, strlen(header), payload, len);
		// 套用到本地
		apply_remote_op((enum LiveOpType)t, line, payload, len);
		if (payload) free(payload);
	}

	// 斷線清理
	pthread_mutex_lock(&live_clients_mutex);
	live_peer_line[cid] = 0;
	live_peer_col[cid] = 0;
	if (idx >= 0 && idx < MAX_PEERS && live_clients[idx].in_use) {
		close(live_clients[idx].fd);
		live_clients[idx].fd = -1;
		live_clients[idx].in_use = 0;
		live_clients[idx].id = 0;
	}
	pthread_mutex_unlock(&live_clients_mutex);
	return NULL;
}

// Host 端：接受新連線
static void *host_accept_thread(void *arg) {
	(void)arg;
	while (live_running) {
		struct sockaddr_in cliaddr;
		socklen_t clilen = sizeof(cliaddr);
		int cfd = accept(live_server_sock, (struct sockaddr *)&cliaddr, &clilen);
		if (cfd < 0) {
			if (!live_running) break;
			continue;
		}
		pthread_mutex_lock(&live_clients_mutex);
		// 超過最大人數則關閉
		if (next_assign_id > MAX_PEERS) {
			pthread_mutex_unlock(&live_clients_mutex);
			close(cfd);
			continue;
		}
		// 找空槽
		int slot = -1;
		for (int i = 0; i < MAX_PEERS; i++) {
			if (!live_clients[i].in_use) { slot = i; break; }
		}
		if (slot == -1) {
			pthread_mutex_unlock(&live_clients_mutex);
			close(cfd);
			continue;
		}
		live_clients[slot].fd = cfd;
		live_clients[slot].id = next_assign_id++;
		live_clients[slot].in_use = 1;
		// 預設新加入者游標未知（0）
		live_peer_line[live_clients[slot].id] = 0;
		int *pidx = (int*)malloc(sizeof(int));
		*pidx = slot;
		pthread_create(&live_clients[slot].thread, NULL, host_client_thread, pidx);
		pthread_mutex_unlock(&live_clients_mutex);
	}
	return NULL;
}

// Client 端：接收主機廣播
static void *live_thread_func(void *arg) {
	(void)arg;
	// 收取循環（client）
	while (live_running) {
		char header[128];
		if (recv_line(live_sock, header, sizeof(header)) <= 0) {
			break;
		}
		int t = 0, line = 0;
		size_t len = 0;
		if (sscanf(header, "OP %d %d %zu", &t, &line, &len) != 3) {
			continue;
		}
		char *payload = NULL;
		if (len > 0) {
			payload = (char *)malloc(len);
			if (!payload) break;
			if (recv_all(live_sock, payload, len) != 0) {
				free(payload);
				break;
			}
		}
		apply_remote_op((enum LiveOpType)t, line, payload, len);
		if (payload) free(payload);
	}
	live_running = 0;
	if (live_sock >= 0) { close(live_sock); live_sock = -1; }
	return NULL;
}

static int live_start_host(int port) {
	live_mode = LIVE_HOST;
	live_self_id = 1;
	live_server_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (live_server_sock < 0) return 0;
	int opt = 1;
	setsockopt(live_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((uint16_t)port);
	if (bind(live_server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 0;
	if (listen(live_server_sock, MAX_PEERS) < 0) return 0;
	live_running = 1;
	if (pthread_create(&live_thread, NULL, host_accept_thread, NULL) != 0) {
		live_running = 0;
		return 0;
	}
	// 主機的游標行先記錄
	if (editors[0].current_line > 0) {
		live_peer_line[live_self_id] = editors[0].current_line;
		live_peer_col[live_self_id] = 0;
	}
	return 1;
}

static int live_start_join(const char *host, int port) {
	live_mode = LIVE_JOIN;
	live_self_id = 0; // 等待主機分配
	live_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (live_sock < 0) return 0;
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
		return 0;
	}
	if (connect(live_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		return 0;
	}
	live_running = 1;
	if (pthread_create(&live_thread, NULL, live_thread_func, NULL) != 0) {
		live_running = 0;
		return 0;
	}
	return 1;
}

static void live_stop() {
	if (live_running) {
		live_running = 0;
		// 關閉 socket 以喚醒阻塞
		if (live_mode == LIVE_JOIN) {
			if (live_sock >= 0) { shutdown(live_sock, SHUT_RDWR); close(live_sock); live_sock = -1; }
			pthread_join(live_thread, NULL);
		} else if (live_mode == LIVE_HOST) {
			// 關閉所有客戶端
			pthread_mutex_lock(&live_clients_mutex);
			for (int i = 0; i < MAX_PEERS; i++) {
				if (live_clients[i].in_use && live_clients[i].fd >= 0) {
					shutdown(live_clients[i].fd, SHUT_RDWR);
					close(live_clients[i].fd);
				}
			}
			pthread_mutex_unlock(&live_clients_mutex);
			// 關閉 listen 並等待接受線程結束
			if (live_server_sock >= 0) { shutdown(live_server_sock, SHUT_RDWR); close(live_server_sock); live_server_sock = -1; }
			pthread_join(live_thread, NULL);
			// 等待所有客戶端線程
			pthread_mutex_lock(&live_clients_mutex);
			for (int i = 0; i < MAX_PEERS; i++) {
				if (live_clients[i].in_use) {
					pthread_join(live_clients[i].thread, NULL);
					live_clients[i].in_use = 0;
					live_clients[i].fd = -1;
					live_clients[i].id = 0;
				}
			}
			pthread_mutex_unlock(&live_clients_mutex);
		}
	}
	live_mode = LIVE_NONE;
}

// 恢復終端設定
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

// 設定終端為原始模式，可以讀取方向鍵
void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// 方向鍵的內部表示（使用不可打印的控制字符避免衝突）
#define KEY_UP         1
#define KEY_DOWN       2
#define KEY_RIGHT      3
#define KEY_LEFT       4
#define KEY_CTRL_LEFT  5
#define KEY_CTRL_RIGHT 6

// 讀取按鍵
char read_key() {
    char c;
    while (read(STDIN_FILENO, &c, 1) != 1);
    
    // 檢測方向鍵（ESC序列）
    if (c == '\033') {
        char seq[6];
        
        // 使用非阻塞模式來判斷是否有後續字符
        struct termios old_term;
        tcgetattr(STDIN_FILENO, &old_term);
        
        struct termios new_term = old_term;
        new_term.c_cc[VMIN] = 0;   // 非阻塞
        new_term.c_cc[VTIME] = 1;  // 0.1 秒超時
        tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
        
        int nread1 = read(STDIN_FILENO, &seq[0], 1);
        int nread2 = 0;
        int nread3 = 0;
        int nread4 = 0;
        int nread5 = 0;
        if (nread1 == 1) {
            nread2 = read(STDIN_FILENO, &seq[1], 1);
        }
        if (nread2 == 1) {
            nread3 = read(STDIN_FILENO, &seq[2], 1);
        }
        if (nread3 == 1) {
            nread4 = read(STDIN_FILENO, &seq[3], 1);
        }
        if (nread4 == 1) {
            nread5 = read(STDIN_FILENO, &seq[4], 1);
        }
        
        // 恢復原始設定
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
        
        // 如果沒有後續字符，這是單純的 ESC
        if (nread1 == 0) {
            return c;  // 返回 ESC
        }
        
        // 檢查是否是 Ctrl + 方向鍵序列 (ESC[1;5C 或 ESC[1;5D)
        if (nread5 == 1 && seq[0] == '[' && seq[1] == '1' && seq[2] == ';' && seq[3] == '5') {
            if (seq[4] == 'C') return KEY_CTRL_RIGHT;  // Ctrl+Right
            if (seq[4] == 'D') return KEY_CTRL_LEFT;   // Ctrl+Left
        }
        
        // 檢查是否是普通方向鍵序列
        if (nread2 == 1 && seq[0] == '[') {
            if (seq[1] == 'A') return KEY_UP;      // Up arrow
            if (seq[1] == 'B') return KEY_DOWN;    // Down arrow
            if (seq[1] == 'C') return KEY_RIGHT;   // Right arrow
            if (seq[1] == 'D') return KEY_LEFT;    // Left arrow
        }
        
        // 不是方向鍵，返回 ESC
        return c;
    }
    
    return c;
}

// 計算總行數
int count_lines(char *buffer) {
    int count = 0;
    char *ptr = buffer;
    
    if (*ptr == '\0') return 0;
    
    count = 1;  // 至少有一行
    while (*ptr) {
        if (*ptr == '\n') {
            count++;
        }
        ptr++;
    }
    
    // 如果最後一個字符是換行符，不要多算一行
    if (*(ptr - 1) == '\n') {
        count--;
    }
    
    return count;
}

// 清除屏幕
void clear_screen() {
    write(STDOUT_FILENO, "\033[2J", 4);
    write(STDOUT_FILENO, "\033[H", 3);
}

// 顯示內容時帶行號（支援視窗滾動）
void print_with_line_numbers(EditorState *ed){
	// 讀取時鎖定，避免網路執行緒同時修改
	int ed_idx = (ed == &editors[0]) ? 0 : 1;
	live_lock_editor(ed_idx);
    char *buffer = ed->buffer;
    int highlight_line = ed->current_line;
    int row_offset = ed->row_offset;
    int total_lines = ed->total_lines;
    
    char *line_start = buffer;
    char *line_end;
    int line_num = 1;
    
    // 先移動到起始行
    while(line_num < row_offset && *line_start){
        line_end = strchr(line_start, '\n');
        if(!line_end) break;
        line_start = line_end + 1;
        line_num++;
    }
    
    printf("\n========== 文件內容 (顯示 %d-%d 行，共 %d 行) ==========\n", 
           row_offset, 
           (row_offset + VISIBLE_LINES - 1 > total_lines) ? total_lines : row_offset + VISIBLE_LINES - 1,
           total_lines);
    
    int displayed_lines = 0;
    while(*line_start && displayed_lines < VISIBLE_LINES){
        line_end = strchr(line_start, '\n');
        int line_length;
        
        if(line_end){
            line_length = line_end - line_start;
        } else {
            line_length = strlen(line_start);
        }
        
		// 前綴：本地或普通
		if(line_num == highlight_line){
			printf("\033[1;32m>>> [行 %d] \033[0m", line_num);  // 本地：綠色加粗
		} else {
			printf("    [行 %d] ", line_num);
		}
		// 準備遠端游標位置（用於在內容中標示），含對應的參與者編號
		int remote_mark_id[512] = {0};     // 該欄位的第一個遠端 ID
		char remote_mark_multi[512] = {0};  // 是否有多個遠端重疊於該欄位
		int remote_eol_id = 0;              // 行尾遠端的第一個 ID
		int remote_eol_multi = 0;           // 行尾是否多個重疊
		if (ed_idx == 0 && live_mode != LIVE_NONE) {
			for (int pid = 1; pid <= MAX_PEERS; pid++) {
				if (pid == live_self_id) continue;
				if (live_peer_line[pid] == line_num) {
					int col = live_peer_col[pid];
					if (col < 0) col = 0;
					if (col >= 511) col = 511;
					if (col >= line_length) { // 行尾
						if (remote_eol_id == 0) remote_eol_id = pid;
						else remote_eol_multi = 1;
					} else {
						if (remote_mark_id[col] == 0) remote_mark_id[col] = pid;
						else remote_mark_multi[col] = 1;
					}
				}
			}
		}
        
        // 先擷取此行內容
        char line_content[512];
        int copy_len = (line_length < 511) ? line_length : 511;
        strncpy(line_content, line_start, copy_len);
        line_content[copy_len] = '\0';
		
		// 準備搜尋匹配標記
		int match_mask[512] = {0}; // 0:無, 1:匹配, 2:當前匹配
		if (ed->search_mode && strlen(ed->search_term) > 0) {
			const char *term = ed->search_term;
			size_t tlen = strlen(term);
			if (tlen > 0) {
				for (int pos = 0; pos + (int)tlen <= copy_len; ) {
					char *found = strstr(&line_content[pos], term);
					if (!found) break;
					int start = (int)(found - line_content);
					int end = start + (int)tlen;
					for (int k = start; k < end && k < 512; k++) {
						match_mask[k] = 1;
					}
					if (line_num == ed->search_result_line && start == ed->search_result_offset) {
						for (int k = start; k < end && k < 512; k++) {
							match_mask[k] = 2;
						}
					}
					pos = end;
				}
			}
		}

		// 逐字輸出，將遠端游標位置直接套用顏色於內容
		for (int i = 0; i < copy_len; i++) {
			int rid = remote_mark_id[i];
			int multi_here = remote_mark_multi[i];
			// 先顯示遠端 ID（若多人重疊則顯示 [+]），不再對後續字元套青色反色
			if (rid || multi_here) {
				if (multi_here) {
					printf("\033[1;36m[+]\033[0m");
				} else {
					printf("\033[1;36m[%d]\033[0m", rid);
				}
			}
			// 僅處理搜尋高亮
			if (match_mask[i] == 2) {
				printf("\033[1;33;7m"); // 當前匹配 黃色反色
			} else if (match_mask[i] == 1) {
				printf("\033[1;33m");   // 其他匹配 黃色
			}
			printf("%c", line_content[i]);
			if (match_mask[i]) {
				printf("\033[0m");
			}
		}
		// 行尾如有遠端游標：僅顯示 ID（或 [+]），不印反色空格
		if (remote_eol_id || remote_eol_multi) {
			if (remote_eol_multi) {
				printf("\033[1;36m[+]\033[0m");
			} else {
				printf("\033[1;36m[%d]\033[0m", remote_eol_id);
			}
		}
        
        if(line_num == highlight_line){
            printf(" \033[1;32m<<<\033[0m\n");  // 行尾也以綠色加粗顯示
        } else {
            printf("\n");
        }
        
        if(!line_end) break;
        line_start = line_end + 1;
        line_num++;
        displayed_lines++;
    }
    
    // 如果顯示的行數不足，填充空白
    while(displayed_lines < VISIBLE_LINES){
        printf("\n");
        displayed_lines++;
    }
    
    printf("====================================================\n\n");
	live_unlock_editor(ed_idx);
}

// 在指定行之後插入新行
void insert_new_line(EditorState *ed, int after_line){
    // 找到插入位置
    char *insert_pos = ed->buffer;
    
    if(after_line > 0){
        for (int i = 0; i < after_line; i++){
            char *next = strchr(insert_pos, '\n');
            if(next){
                insert_pos = next + 1;
            } else {
                // 如果到了文件末尾但沒有換行符，先添加一個
                insert_pos = ed->buffer + strlen(ed->buffer);
                break;
            }
        }
    }
    
    // 保存插入位置之後的內容
    char after_content[512] = {0};
    strcpy(after_content, insert_pos);
    
    // 在插入位置添加新行（空行加換行符）
    strcpy(insert_pos, "\n");
    strcpy(insert_pos + 1, after_content);
    
    // 推入逆操作：刪除新插入的行
    push_undo(ed, UNDO_DELETE_LINE, after_line + 1, NULL);

    // printf("\n✓ 已在第 %d 行之後插入新行\n", after_line);
    // printf("按任意鍵繼續...");
    // read_key();
	// 廣播（不帶 payload 的插入）
	live_broadcast_simple(OP_INSERT_AFTER, after_line);
}

// 刪除指定行
int delete_line(EditorState *ed, int line_to_delete){
    // 如果文件只有一行，不允許刪除
    int total = count_lines(ed->buffer);
    if(total <= 1){
        printf("\n✗ 無法刪除：文件至少需要保留一行\n");
        printf("按任意鍵繼續...");
        read_key();
        return 0;  // 刪除失敗
    }
    
    // 找到要刪除的行的起始位置
    char *line_start = ed->buffer;
    for(int i = 0; i < line_to_delete - 1; i++){
        char *next = strchr(line_start, '\n');
        if(next){
            line_start = next + 1;
        } else {
            printf("\n✗ 錯誤：找不到指定行\n");
            printf("按任意鍵繼續...");
            read_key();
            return 0;
        }
    }
    
    // 找到要刪除的行的結束位置（下一個換行符）
    char *line_end = strchr(line_start, '\n');
    
    // 保存將被刪除的內容（不包含換行）
    char deleted_content[512] = {0};
    int line_length = 0;
    if(line_end){
        line_length = (int)(line_end - line_start);
    } else {
        line_length = (int)strlen(line_start);
    }
    if (line_length > 511) line_length = 511;
    strncpy(deleted_content, line_start, line_length);
    deleted_content[line_length] = '\0';

    // 保存要刪除行之後的內容
    char after_content[512] = {0};
    if(line_end){
        // 如果找到換行符，保存換行符之後的所有內容
        strcpy(after_content, line_end + 1);
        // 將刪除行之後的內容複製到刪除行的起始位置
        strcpy(line_start, after_content);
    } else {
        // 如果這是最後一行且沒有換行符
        // 需要刪除前一個換行符
        if(line_start > ed->buffer && *(line_start - 1) == '\n'){
            *(line_start - 1) = '\0';
        } else {
            *line_start = '\0';
        }
    }
    
    // 推入逆操作：在前一行之後插回被刪除的內容
    push_undo(ed, UNDO_INSERT_AFTER_WITH_CONTENT, line_to_delete - 1, deleted_content);

    // printf("\n✓ 已刪除第 %d 行\n", line_to_delete);
    // printf("按任意鍵繼續...");
    // read_key();

	// 廣播刪除
	live_broadcast_simple(OP_DELETE_LINE, line_to_delete);
    return 1;  // 刪除成功
}

// 複製指定行到剪貼板
void copy_line(char *buffer, int line_to_copy){
    // 找到要複製的行的起始位置
    char *line_start = buffer;
    for(int i = 0; i < line_to_copy - 1; i++){
        char *next = strchr(line_start, '\n');
        if(next){
            line_start = next + 1;
        } else {
            printf("\n✗ 錯誤：找不到指定行\n");
            printf("按任意鍵繼續...");
            read_key();
            return;
        }
    }
    
    // 找到行的結束位置
    char *line_end = strchr(line_start, '\n');
    int line_length;
    
    if(line_end){
        line_length = line_end - line_start;
    } else {
        line_length = strlen(line_start);
    }
    
    // 複製到剪貼板（不包括換行符）
    if(line_length >= 512){
        line_length = 511;  // 防止緩衝區溢出
    }
    
    strncpy(clipboard, line_start, line_length);
    clipboard[line_length] = '\0';
    clipboard_has_content = 1;
    
    // printf("\n✓ 已複製第 %d 行到剪貼板\n", line_to_copy);
    // printf("內容：%s\n", clipboard);
    // printf("按任意鍵繼續...");
    // read_key();
}

// 將剪貼板內容貼上到指定行之後
void paste_line(EditorState *ed, int after_line){
    if(!clipboard_has_content){
        printf("\n✗ 剪貼板為空，請先複製內容\n");
        printf("按任意鍵繼續...");
        read_key();
        return;
    }
    
    // 找到插入位置（在指定行之後）
    char *insert_pos = ed->buffer;
    
    if(after_line > 0){
        for(int i = 0; i < after_line; i++){
            char *next = strchr(insert_pos, '\n');
            if(next){
                insert_pos = next + 1;
            } else {
                // 如果到了文件末尾但沒有換行符，先添加一個
                insert_pos = ed->buffer + strlen(ed->buffer);
                break;
            }
        }
    }
    
    // 保存插入位置之後的內容
    char after_content[512] = {0};
    strcpy(after_content, insert_pos);
    
    // 在插入位置添加剪貼板內容和換行符
    strcpy(insert_pos, clipboard);
    strcpy(insert_pos + strlen(clipboard), "\n");
    strcpy(insert_pos + strlen(clipboard) + 1, after_content);
    
    // 推入逆操作：刪除新貼上的行
    push_undo(ed, UNDO_DELETE_LINE, after_line + 1, NULL);

    // printf("\n✓ 已在第 %d 行之後貼上內容\n", after_line);
    // printf("內容：%s\n", clipboard);
    // printf("按任意鍵繼續...");
    // read_key();
	// 廣播貼上（帶內容）
	live_broadcast_with_payload(OP_PASTE_AFTER, after_line, clipboard);
}

// 計算總共有多少個匹配
int count_matches(char *buffer, const char *search_term) {
    if(strlen(search_term) == 0) return 0;
    
    int count = 0;
    char *ptr = buffer;
    
    while((ptr = strstr(ptr, search_term)) != NULL) {
        count++;
        ptr += strlen(search_term);
    }
    
    return count;
}

// 搜尋指定字串，從指定位置開始
// 返回值：1=找到，0=未找到
int search_forward(char *buffer, const char *search_term, int start_line, int start_offset,
                  int *result_line, int *result_offset) {
    if(strlen(search_term) == 0) return 0;
    
    char *line_start = buffer;
    int current_line = 1;
    
    // 移動到起始行
    while(current_line < start_line && *line_start) {
        char *next = strchr(line_start, '\n');
        if(!next) break;
        line_start = next + 1;
        current_line++;
    }
    
    // 從起始偏移量開始搜尋
    char *search_start = line_start + start_offset;
    char *found = strstr(search_start, search_term);
    
    // 如果在當前行沒找到，繼續搜尋後面的行
    if(!found || (strchr(search_start, '\n') && found > strchr(search_start, '\n'))) {
        // 移到下一行
        char *next_line = strchr(line_start, '\n');
        if(next_line) {
            line_start = next_line + 1;
            current_line++;
            
            // 搜尋剩餘的行
            while(*line_start) {
                found = strstr(line_start, search_term);
                char *line_end = strchr(line_start, '\n');
                
                if(found && (!line_end || found < line_end)) {
                    *result_line = current_line;
                    *result_offset = found - line_start;
                    return 1;
                }
                
                if(!line_end) break;
                line_start = line_end + 1;
                current_line++;
            }
        }
    } else {
        // 在當前行找到
        *result_line = current_line;
        *result_offset = found - line_start;
        return 1;
    }
    
    // 沒找到，從頭開始循環搜尋
    line_start = buffer;
    current_line = 1;
    
    while(current_line < start_line) {
        found = strstr(line_start, search_term);
        char *line_end = strchr(line_start, '\n');
        
        if(found && (!line_end || found < line_end)) {
            *result_line = current_line;
            *result_offset = found - line_start;
            return 1;
        }
        
        if(!line_end) break;
        line_start = line_end + 1;
        current_line++;
    }
    
    return 0;  // 完全沒找到
}

// 進入搜尋模式，讓用戶輸入搜尋字串（顯示文本內容）
void enter_search_mode(EditorState *ed) {
    clear_screen();
    
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║              搜尋模式                     ║\n");
    printf("╚═══════════════════════════════════════════╝\n");
    
    // 顯示當前文本內容，讓用戶參考
    print_with_line_numbers(ed);
    
    printf("\n");
    printf("┌─────────────────────────────────────────┐\n");
    printf("│ 請輸入要搜尋的字串：");
    
    // 臨時禁用原始模式以便讀取一行文字
    disable_raw_mode();
    
    if(fgets(ed->search_term, sizeof(ed->search_term), stdin) != NULL) {
        // 移除換行符
        size_t len = strlen(ed->search_term);
        if(len > 0 && ed->search_term[len-1] == '\n') {
            ed->search_term[len-1] = '\0';
        }
        
        if(strlen(ed->search_term) > 0) {
            ed->search_mode = 1;
            ed->current_match = 0;
        }
    }
    
    printf("└─────────────────────────────────────────┘\n");
    
    // 重新啟用原始模式
    enable_raw_mode();
}

void edit_line(EditorState *ed){
    char *buffer = ed->buffer;
    int current_line = ed->current_line;
    
	//（改至取得初始欄位位置後再廣播）

    // 找到要編輯的行
    char *line_ptr = buffer;
    for (int i = 0; i < current_line - 1; i++){
        line_ptr = strchr(line_ptr, '\n') + 1;
    }
    
    // 找到行尾
    char *line_end = strchr(line_ptr, '\n');
    int line_length = line_end ? (int)(line_end - line_ptr) : (int)strlen(line_ptr);
    
    // 複製當前行內容到臨時緩衝區
    char line_content[512] = {0};
    strncpy(line_content, line_ptr, line_length);
    
    // 保存行後面的內容
    char after_line[512] = {0};
    if(line_end){
        strcpy(after_line, line_end);
    }
    
    int cursor_pos = line_length;  // 光標位置（從行尾開始）
    int content_len = line_length;
	// 進入編輯時廣播目前行號與欄位
	live_broadcast_cursor(current_line, cursor_pos);
    
    // 編輯循環
    while(1){
        // 清除屏幕並顯示編輯界面
        clear_screen();
        printf("╔═══════════════════════════════════════════╗\n");
        printf("║       編輯模式 - 行 %d                    ║\n", current_line);
        printf("╚═══════════════════════════════════════════╝\n");
        
        // 顯示文本內容讓用戶參考
        print_with_line_numbers(ed);
        
        printf("\n");
        printf("操作說明：[←/→] 移動光標  [Backspace] 刪除  [Enter] 完成  [ESC] 取消\n\n");
        
        printf("編輯第 %d 行：\n", current_line);
        printf("┌─────────────────────────────────────────┐\n");
        printf("│ ");
        
        // 顯示內容，在光標位置顯示特殊標記
        for(int i = 0; i < content_len; i++){
            if(i == cursor_pos){
                printf("\033[7m"); // 反色顯示光標位置
            }
            printf("%c", line_content[i]);
            if(i == cursor_pos){
                printf("\033[0m"); // 重置顏色
            }
        }
        
        // 如果光標在最後，顯示空格光標
        if(cursor_pos == content_len){
            printf("\033[7m \033[0m");
        }
        
        printf("\n└─────────────────────────────────────────┘\n");
        
        // 讀取按鍵
        char key = read_key();
        
        if(key == '\r' || key == '\n'){
            // Enter - 完成編輯
            line_content[content_len] = '\0';
			// 推入逆操作：記錄原始行內容
			{
				char orig_content[512] = {0};
				int orig_len = line_length < 511 ? line_length : 511;
				strncpy(orig_content, line_ptr, orig_len);
				orig_content[orig_len] = '\0';
				push_undo(ed, UNDO_SET_LINE, current_line, orig_content);
			}
			// 寫入時短暫上鎖
			live_lock_editor((ed == &editors[0]) ? 0 : 1);
			strcpy(line_ptr, line_content);
			strcpy(line_ptr + content_len, after_line);
			live_unlock_editor((ed == &editors[0]) ? 0 : 1);
			// 廣播更新此行
			live_broadcast_with_payload(OP_EDIT_LINE, current_line, line_content);
            break;
        }
        else if(key == '\033'){
            // ESC - 取消編輯
            // clear_screen();
            // printf("\n✗ 已取消編輯\n");
            // printf("按任意鍵繼續...");
            // read_key();
            break;
        }
        else if(key == KEY_LEFT){
            // 左移光標
            if(cursor_pos > 0){
                cursor_pos--;
				live_broadcast_cursor(current_line, cursor_pos);
            }
        }
        else if(key == KEY_RIGHT){
            // 右移光標
            if(cursor_pos < content_len){
                cursor_pos++;
				live_broadcast_cursor(current_line, cursor_pos);
            }
        }
        else if(key == 127 || key == '\b'){
            // Backspace - 刪除光標前的字符
            if(cursor_pos > 0){
                // 將光標後的內容前移
                for(int i = cursor_pos - 1; i < content_len; i++){
                    line_content[i] = line_content[i + 1];
                }
                cursor_pos--;
                content_len--;
				live_broadcast_cursor(current_line, cursor_pos);
            }
        }
        else if(key >= 32 && key <= 126){
            // 可打印字符 - 在光標位置插入
            if(content_len < 510){
                // 將光標後的內容後移
                for(int i = content_len; i > cursor_pos; i--){
                    line_content[i] = line_content[i - 1];
                }
                line_content[cursor_pos] = key;
                cursor_pos++;
                content_len++;
				live_broadcast_cursor(current_line, cursor_pos);
            }
        }
    }
}

// 保存編輯器狀態到文件
void save_editor(EditorState *ed) {
    FILE *file = fopen(ed->filename, "w");
    if(file) {
		// 寫入前鎖定，避免與網路執行緒衝突
		live_lock_editor((ed == &editors[0]) ? 0 : 1);
		fwrite(ed->buffer, strlen(ed->buffer), 1, file);
		live_unlock_editor((ed == &editors[0]) ? 0 : 1);
        fclose(file);
    }
}

// 初始化編輯器狀態
int init_editor(EditorState *ed, const char *filename) {
    strncpy(ed->filename, filename, sizeof(ed->filename) - 1);
    ed->filename[sizeof(ed->filename) - 1] = '\0';
    
    FILE *file = fopen(filename, "r");
    if(!file) {
        printf("無法打開文件: %s\n", filename);
        return 0;
    }
    
    memset(ed->buffer, 0, sizeof(ed->buffer));
    fread(ed->buffer, 1, sizeof(ed->buffer) - 1, file);
    fclose(file);
    
    ed->total_lines = count_lines(ed->buffer);
    if(ed->total_lines == 0) {
        printf("文件為空: %s\n", filename);
        return 0;
    }
    
    ed->current_line = 1;
    ed->row_offset = 1;
    ed->search_mode = 0;
    ed->search_term[0] = '\0';
    ed->search_result_line = 0;
    ed->search_result_offset = 0;
    ed->total_matches = 0;
    ed->current_match = 0;
    
    return 1;
}

int main(int argc,char **argv){

	int argi = 1;
	const char *join_host = NULL;
	int join_port = 0;
	int host_port = 0;

	// 參數解析： [--host PORT | --join HOST:PORT] <filename1> [filename2]
	if (argc >= 3 && strcmp(argv[argi], "--host") == 0) {
		host_port = atoi(argv[argi + 1]);
		argi += 2;
	} else if (argc >= 3 && strcmp(argv[argi], "--join") == 0) {
		char *hp = argv[argi + 1];
		char *colon = strchr(hp, ':');
		if (colon) {
			*colon = '\0';
			join_host = hp;
			join_port = atoi(colon + 1);
			argi += 2;
		} else {
			printf("使用方式: %s [--host PORT | --join HOST:PORT] <filename1> [filename2]\n", argv[0]);
			return 1;
		}
	}

	if(argc - argi < 1){
		printf("使用方式: %s [--host PORT | --join HOST:PORT] <filename1> [filename2]\n", argv[0]);
		printf("  filename1: 第一個要編輯的文件\n");
		printf("  filename2: (可選) 第二個要編輯的文件\n");
		printf("  使用 Ctrl+左/右 鍵在兩個文件間切換\n");
		printf("  Live Share: --host 啟動主機；--join 以 HOST:PORT 連線\n");
		return 1;
	}

    // 初始化編輯器
	num_editors = ((argc - argi) >= 2) ? 2 : 1;
    
	if(!init_editor(&editors[0], argv[argi])) {
        return 1;
    }
    
	if(num_editors == 2) {;
		if(!init_editor(&editors[1], argv[argi + 1])) {
            return 1;
        }
    }
    
    active_editor = 0;

	// 啟動 Live Share（若有要求）
	if (host_port > 0) {
		if (!live_start_host(host_port)) {
			printf("Live Share 主機啟動失敗（port=%d）\n", host_port);
		} else {
			printf("Live Share 主機啟動中，等待連線（port=%d）...\n", host_port);
		}
	} else if (join_host && join_port > 0) {
		if (!live_start_join(join_host, join_port)) {
			printf("Live Share 無法連線到 %s:%d\n", join_host, join_port);
		} else {
			printf("Live Share 已連線到 %s:%d\n", join_host, join_port);
		}
	}
    
    // 啟用原始模式來讀取方向鍵
    enable_raw_mode();
    
    clear_screen();
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║       文本編輯器 - 鍵盤導航模式          ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");
    printf("操作說明：\n");
    printf("  ↑/↓     - 上下移動選擇行\n");
    printf("  Enter   - 進入編輯模式\n");
    printf("  f       - 搜尋字串\n");
    printf("  n       - 在當前行之後新增一行 / 搜尋模式下跳到下一個匹配\n");
    printf("  d       - 刪除當前行\n");
    printf("  c       - 複製當前行\n");
    printf("  p       - 貼上複製的內容\n");
    printf("  u       - 復原上一個動作\n");
    if(num_editors == 2) {
        printf("  Ctrl+←/→ - 切換視窗\n");
    }
    printf("  q       - 退出編輯器\n\n");
	if (live_mode == LIVE_HOST) {
		printf("[Live Share] 角色：主機（等待/已連線）\n");
	} else if (live_mode == LIVE_JOIN) {
		printf("[Live Share] 角色：加入（已連線）\n");
	}
    printf("編輯模式功能：\n");
    printf("  ←/→      - 左右移動光標\n");
    printf("  字符輸入  - 在光標位置插入\n");
    printf("  Backspace - 刪除字符\n\n");
    printf("按任意鍵開始...\n");
    read_key();
    
    // 主循環
    while(1){
        EditorState *ed = &editors[active_editor];
        clear_screen();
        
        // 顯示標題
        if(num_editors == 2) {
            printf("╔═══════════════════════════════════════════╗\n");
            printf("║  視窗 %d/%d: %-32s║\n", active_editor + 1, num_editors, ed->filename);
            printf("╚═══════════════════════════════════════════╝\n");
        } else {
            printf("╔═══════════════════════════════════════════╗\n");
            printf("║  文件: %-35s║\n", ed->filename);
            printf("╚═══════════════════════════════════════════╝\n");
        }
		if (live_mode != LIVE_NONE) {
			printf("[Live Share] 模式: %s\n", live_mode == LIVE_HOST ? "主機" : "加入");
		}
        
        // 顯示文件內容，高亮當前行
        print_with_line_numbers(ed);
        
        // 顯示提示信息
        printf("\n");
        printf("當前選擇：第 %d 行 (共 %d 行)%s%s ", 
               ed->current_line, ed->total_lines, 
               clipboard_has_content ? "  [剪貼板:" : "",
               ed->search_mode ? "  [搜尋: " : "");
        if(ed->search_mode) {
            printf("%s] (%d/%d)", ed->search_term, ed->current_match, ed->total_matches);
        }
		if (clipboard_has_content) {
			// 顯示剪貼板內容預覽（最多 40 字）
			int clip_len = (int)strlen(clipboard);
			int show_len = (clip_len > 40) ? 40 : clip_len;
			char clip_preview[64] = {0};
			strncpy(clip_preview, clipboard, show_len);
			clip_preview[show_len] = '\0';
			printf(" %s%s]", clip_preview, (clip_len > show_len) ? "..." : "");
		}
        printf("\n");
        if(ed->search_mode) {
            printf("操作：[n] 下一個匹配  [ESC] 退出搜尋  [↑↓] 移動  [Enter] 編輯  [q] 退出\n");
        } else {
            if(num_editors == 2) {
                printf("操作：[f] 搜尋  [↑↓] 移動  [Enter] 編輯  [n] 新增  [d] 刪除  [c] 複製  [p] 貼上  [u] 復原  [Ctrl+←/→] 切換  [q] 退出\n");
            } else {
                printf("操作：[f] 搜尋  [↑↓] 移動  [Enter] 編輯  [n] 新增  [d] 刪除  [c] 複製  [p] 貼上  [u] 復原  [q] 退出\n");
            }
        }
        
        // 讀取按鍵
        char key = read_key();
        
        // 處理視窗切換
        if(num_editors == 2 && (key == KEY_CTRL_LEFT || key == KEY_CTRL_RIGHT)) {
            if(key == KEY_CTRL_RIGHT) {
                active_editor = (active_editor + 1) % num_editors;
            } else if(key == KEY_CTRL_LEFT) {
                active_editor = (active_editor - 1 + num_editors) % num_editors;
            }
            continue;
        }
        
        if(key == 'f' || key == 'F'){  // F 鍵
            // 進入搜尋模式（顯示當前文本內容）
            enter_search_mode(ed);
            
            if(ed->search_mode && strlen(ed->search_term) > 0) {
                // 計算總匹配數
                ed->total_matches = count_matches(ed->buffer, ed->search_term);
                
                if(ed->total_matches > 0) {
                    // 從當前位置開始搜尋第一個匹配
                    if(search_forward(ed->buffer, ed->search_term, ed->current_line, 0,
                                    &ed->search_result_line, &ed->search_result_offset)) {
                        ed->current_line = ed->search_result_line;
                        ed->current_match = 1;
                        
                        // 調整視窗位置
                        if(ed->current_line < ed->row_offset) {
                            ed->row_offset = ed->current_line;
                        } else if(ed->current_line >= ed->row_offset + VISIBLE_LINES) {
                            ed->row_offset = ed->current_line - VISIBLE_LINES + 1;
                        }
						// 廣播游標位置（非編輯模式，欄位以 0 表示）
						live_broadcast_cursor(ed->current_line, 0);
                    }
                } else {
                    ed->search_mode = 0;
                    clear_screen();
                    printf("\n✗ 未找到匹配的結果\n");
                    printf("按任意鍵繼續...");
                    read_key();
                }
            }
        }
        else if(key == '\033'){  // ESC 鍵
            if(ed->search_mode){
                // 退出搜尋模式
                ed->search_mode = 0;
                ed->search_term[0] = '\0';
                ed->total_matches = 0;
                ed->current_match = 0;
                ed->search_result_line = 0;
                ed->search_result_offset = 0;
            }
        }
        else if(key == 'q' || key == 'Q'){
            // 退出
            clear_screen();
            disable_raw_mode();
			live_stop();
            printf("\n正在退出編輯器...\n");
            break;
        }
        else if(key == KEY_UP){
            // 上移
            if(ed->current_line > 1){
                ed->current_line--;
                // 如果當前行移出視窗頂部，調整視窗
                if(ed->current_line < ed->row_offset){
                    ed->row_offset = ed->current_line;
                }
				// 廣播游標位置（非編輯模式，欄位以 0 表示）
				live_broadcast_cursor(ed->current_line, 0);
            }
        }
        else if(key == KEY_DOWN){
            // 下移
            if(ed->current_line < ed->total_lines){
                ed->current_line++;
                // 如果當前行移出視窗底部，調整視窗
                if(ed->current_line >= ed->row_offset + VISIBLE_LINES){
                    ed->row_offset = ed->current_line - VISIBLE_LINES + 1;
                }
				// 廣播游標位置（非編輯模式，欄位以 0 表示）
				live_broadcast_cursor(ed->current_line, 0);
            }
        }
        else if(key == 'n' || key == 'N'){
            if(ed->search_mode && strlen(ed->search_term) > 0) {
                // 搜尋模式：跳到下一個匹配
                int next_offset = ed->search_result_offset + strlen(ed->search_term);
                
                if(search_forward(ed->buffer, ed->search_term, ed->search_result_line, next_offset,
                                &ed->search_result_line, &ed->search_result_offset)) {
                    ed->current_line = ed->search_result_line;
                    ed->current_match++;
                    if(ed->current_match > ed->total_matches) {
                        ed->current_match = 1;  // 循環回第一個
                    }
                    
                    // 調整視窗位置
                    if(ed->current_line < ed->row_offset) {
                        ed->row_offset = ed->current_line;
                    } else if(ed->current_line >= ed->row_offset + VISIBLE_LINES) {
                        ed->row_offset = ed->current_line - VISIBLE_LINES + 1;
                    }
					// 廣播游標位置（非編輯模式，欄位以 0 表示）
					live_broadcast_cursor(ed->current_line, 0);
                }
            } else {
                // 非搜尋模式：在當前行之後新增一行
                clear_screen();
                print_with_line_numbers(ed);

                insert_new_line(ed, ed->current_line);
                
                // 自動保存
                save_editor(ed);
                
                // 重新計算行數
                ed->total_lines = count_lines(ed->buffer);

                // 移動到新插入的行
                ed->current_line++;
                if(ed->current_line > ed->total_lines){
                    ed->current_line = ed->total_lines;
                }
                // 調整視窗位置
                if(ed->current_line >= ed->row_offset + VISIBLE_LINES){
                    ed->row_offset = ed->current_line - VISIBLE_LINES + 1;
                }
				// 廣播游標位置（非編輯模式，欄位以 0 表示）
				live_broadcast_cursor(ed->current_line, 0);
            }
        }
        else if(key == 'd' || key == 'D'){
            // 刪除當前行
            clear_screen();
            print_with_line_numbers(ed);
            
            // 嘗試刪除當前行
            int deleted = delete_line(ed, ed->current_line);
            
            if(deleted){
                // 自動保存
                save_editor(ed);
                
                // 重新計算行數
                ed->total_lines = count_lines(ed->buffer);
                
                // 調整當前行位置
                if(ed->current_line > ed->total_lines){
                    ed->current_line = ed->total_lines;
                }
                if(ed->current_line < 1){
                    ed->current_line = 1;
                }
                
                // 調整視窗位置
                if(ed->current_line < ed->row_offset){
                    ed->row_offset = ed->current_line;
                }
                if(ed->current_line >= ed->row_offset + VISIBLE_LINES){
                    ed->row_offset = ed->current_line - VISIBLE_LINES + 1;
                }
				// 廣播游標位置（非編輯模式，欄位以 0 表示）
				live_broadcast_cursor(ed->current_line, 0);
            }
        }
        else if(key == 'c' || key == 'C'){
            // 複製當前行
            clear_screen();
            print_with_line_numbers(ed);
            
            copy_line(ed->buffer, ed->current_line);
        }
        else if(key == 'p' || key == 'P'){
            // 貼上複製的內容到當前行之後
            clear_screen();
            print_with_line_numbers(ed);
            
            paste_line(ed, ed->current_line);
            
            // 自動保存
            save_editor(ed);
            
            // 重新計算行數
            ed->total_lines = count_lines(ed->buffer);
            
            // 移動到新貼上的行
            if(clipboard_has_content){
                ed->current_line++;
                if(ed->current_line > ed->total_lines){
                    ed->current_line = ed->total_lines;
                }
                // 調整視窗位置
                if(ed->current_line >= ed->row_offset + VISIBLE_LINES){
                    ed->row_offset = ed->current_line - VISIBLE_LINES + 1;
                }
                // 廣播游標位置（非編輯模式，欄位以 0 表示）
                live_broadcast_cursor(ed->current_line, 0);
            }
        }
        else if(key == 'u' || key == 'U'){
            // 復原上一個動作
            clear_screen();
            print_with_line_numbers(ed);
            undo_last_action(ed);
            // 狀態已在復原過程中更新並保存，這裡再保險一次視窗邊界
            editor_recount_and_clamp(ed);
        }
        else if(key == KEY_LEFT || key == KEY_RIGHT){
            // 左右方向鍵在主選單中不執行任何操作（僅在編輯模式中使用）
            // 忽略這些按鍵，避免未處理的輸入
        }
        else if(key == '\r' || key == '\n'){
            // Enter - 編輯當前行
            edit_line(ed);
            
            // 自動保存
            save_editor(ed);
            
            // 重新計算行數（可能有變化）
            ed->total_lines = count_lines(ed->buffer);
            if(ed->current_line > ed->total_lines){
                ed->current_line = ed->total_lines;
            }
        }
    }
    
    // 最終保存所有編輯器
    for(int i = 0; i < num_editors; i++) {
        save_editor(&editors[i]);
    }
    
    if(num_editors == 2) {
        printf("文件已保存並退出:\n");
        printf("  - %s\n", editors[0].filename);
        printf("  - %s\n", editors[1].filename);
    } else {
        printf("文件已保存並退出: %s\n", editors[0].filename);
    }
    printf("再見！\n\n");

    return 0;
}