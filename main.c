#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>

struct termios orig_termios;

#define VISIBLE_LINES 15  // 一次顯示的行數

// 剪貼板緩衝區（用於複製貼上）
char clipboard[512] = {0};
int clipboard_has_content = 0;  // 標記剪貼板是否有內容

// 編輯器狀態結構體（每個文件一個）
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
} EditorState;

// 全局變數
EditorState editors[2];  // 最多兩個編輯器
int num_editors = 0;     // 實際編輯器數量（1或2）
int active_editor = 0;   // 當前活動的編輯器（0或1）

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
        
        // 如果是要編輯的行，用特殊標記顯示
        if(line_num == highlight_line){
            printf("\033[1;32m>>> [行 %d] ", line_num);  // 綠色加粗
        } else {
            printf("    [行 %d] ", line_num);
        }
        
        // 如果在搜尋模式且這行有匹配，高亮顯示搜尋詞
        if(ed->search_mode && strlen(ed->search_term) > 0) {
            char line_content[512];
            int copy_len = (line_length < 511) ? line_length : 511;
            strncpy(line_content, line_start, copy_len);
            line_content[copy_len] = '\0';
            
            char *match_pos = line_content;
            char *last_pos = line_content;
            int printed = 0;
            
            while((match_pos = strstr(match_pos, ed->search_term)) != NULL) {
                // 打印匹配前的部分
                printf("%.*s", (int)(match_pos - last_pos), last_pos);
                // 高亮打印匹配的部分
                if(line_num == ed->search_result_line && 
                   (match_pos - line_content) == ed->search_result_offset) {
                    printf("\033[1;33;7m%.*s\033[0m", (int)strlen(ed->search_term), match_pos);  // 黃色反色（當前匹配）
                } else {
                    printf("\033[1;33m%.*s\033[0m", (int)strlen(ed->search_term), match_pos);  // 黃色（其他匹配）
                }
                match_pos += strlen(ed->search_term);
                last_pos = match_pos;
                printed = 1;
            }
            
            if(printed) {
                // 打印剩餘的部分
                printf("%s", last_pos);
            } else {
                // 沒有匹配，正常打印
                printf("%.*s", line_length, line_start);
            }
        } else {
            // 正常打印
            printf("%.*s", line_length, line_start);
        }
        
        if(line_num == highlight_line){
            printf(" <<<\033[0m\n");  // 重置顏色
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
}

// 在指定行之後插入新行
void insert_new_line(char *buffer, int after_line){
    // 找到插入位置
    char *insert_pos = buffer;
    
    if(after_line > 0){
        for (int i = 0; i < after_line; i++){
            char *next = strchr(insert_pos, '\n');
            if(next){
                insert_pos = next + 1;
            } else {
                // 如果到了文件末尾但沒有換行符，先添加一個
                insert_pos = buffer + strlen(buffer);
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
    
    printf("\n✓ 已在第 %d 行之後插入新行\n", after_line);
    printf("按任意鍵繼續...");
    read_key();
}

// 刪除指定行
int delete_line(char *buffer, int line_to_delete){
    // 如果文件只有一行，不允許刪除
    int total = count_lines(buffer);
    if(total <= 1){
        printf("\n✗ 無法刪除：文件至少需要保留一行\n");
        printf("按任意鍵繼續...");
        read_key();
        return 0;  // 刪除失敗
    }
    
    // 找到要刪除的行的起始位置
    char *line_start = buffer;
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
        if(line_start > buffer && *(line_start - 1) == '\n'){
            *(line_start - 1) = '\0';
        } else {
            *line_start = '\0';
        }
    }
    
    printf("\n✓ 已刪除第 %d 行\n", line_to_delete);
    printf("按任意鍵繼續...");
    read_key();
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
    
    printf("\n✓ 已複製第 %d 行到剪貼板\n", line_to_copy);
    printf("內容：%s\n", clipboard);
    printf("按任意鍵繼續...");
    read_key();
}

// 將剪貼板內容貼上到指定行之後
void paste_line(char *buffer, int after_line){
    if(!clipboard_has_content){
        printf("\n✗ 剪貼板為空，請先複製內容\n");
        printf("按任意鍵繼續...");
        read_key();
        return;
    }
    
    // 找到插入位置（在指定行之後）
    char *insert_pos = buffer;
    
    if(after_line > 0){
        for(int i = 0; i < after_line; i++){
            char *next = strchr(insert_pos, '\n');
            if(next){
                insert_pos = next + 1;
            } else {
                // 如果到了文件末尾但沒有換行符，先添加一個
                insert_pos = buffer + strlen(buffer);
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
    
    printf("\n✓ 已在第 %d 行之後貼上內容\n", after_line);
    printf("內容：%s\n", clipboard);
    printf("按任意鍵繼續...");
    read_key();
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
            strcpy(line_ptr, line_content);
            strcpy(line_ptr + content_len, after_line);
            
            clear_screen();
            printf("\n✓ 已更新行 %d\n", current_line);
            printf("按任意鍵繼續...");
            read_key();
            break;
        }
        else if(key == '\033'){
            // ESC - 取消編輯
            clear_screen();
            printf("\n✗ 已取消編輯\n");
            printf("按任意鍵繼續...");
            read_key();
            break;
        }
        else if(key == KEY_LEFT){
            // 左移光標
            if(cursor_pos > 0){
                cursor_pos--;
            }
        }
        else if(key == KEY_RIGHT){
            // 右移光標
            if(cursor_pos < content_len){
                cursor_pos++;
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
            }
        }
    }
}

// 保存編輯器狀態到文件
void save_editor(EditorState *ed) {
    FILE *file = fopen(ed->filename, "w");
    if(file) {
        fwrite(ed->buffer, strlen(ed->buffer), 1, file);
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

    if(argc < 2){
        printf("使用方式: %s <filename1> [filename2]\n", argv[0]);
        printf("  filename1: 第一個要編輯的文件\n");
        printf("  filename2: (可選) 第二個要編輯的文件\n");
        printf("  使用 Ctrl+左/右 鍵在兩個文件間切換\n");
        return 1;
    }

    // 初始化編輯器
    num_editors = (argc >= 3) ? 2 : 1;
    
    if(!init_editor(&editors[0], argv[1])) {
        return 1;
    }
    
    if(num_editors == 2) {
        if(!init_editor(&editors[1], argv[2])) {
            return 1;
        }
    }
    
    active_editor = 0;
    
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
    if(num_editors == 2) {
        printf("  Ctrl+←/→ - 切換視窗\n");
    }
    printf("  q       - 退出編輯器\n\n");
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
        
        // 顯示文件內容，高亮當前行
        print_with_line_numbers(ed);
        
        // 顯示提示信息
        printf("\n");
        printf("當前選擇：第 %d 行 (共 %d 行)%s%s\n", 
               ed->current_line, ed->total_lines, 
               clipboard_has_content ? "  [剪貼板: ✓]" : "",
               ed->search_mode ? "  [搜尋: " : "");
        if(ed->search_mode) {
            printf("%s] (%d/%d)", ed->search_term, ed->current_match, ed->total_matches);
        }
        printf("\n");
        if(ed->search_mode) {
            printf("操作：[n] 下一個匹配  [ESC] 退出搜尋  [↑↓] 移動  [Enter] 編輯  [q] 退出\n");
        } else {
            if(num_editors == 2) {
                printf("操作：[f] 搜尋  [↑↓] 移動  [Enter] 編輯  [n] 新增  [d] 刪除  [c] 複製  [p] 貼上  [Ctrl+←/→] 切換  [q] 退出\n");
            } else {
                printf("操作：[f] 搜尋  [↑↓] 移動  [Enter] 編輯  [n] 新增  [d] 刪除  [c] 複製  [p] 貼上  [q] 退出\n");
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
                }
            } else {
                // 非搜尋模式：在當前行之後新增一行
                clear_screen();
                print_with_line_numbers(ed);

                insert_new_line(ed->buffer, ed->current_line);
                
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
            }
        }
        else if(key == 'd' || key == 'D'){
            // 刪除當前行
            clear_screen();
            print_with_line_numbers(ed);
            
            // 嘗試刪除當前行
            int deleted = delete_line(ed->buffer, ed->current_line);
            
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
            
            paste_line(ed->buffer, ed->current_line);
            
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
            }
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
        printf("✓ 文件已保存並退出:\n");
        printf("  - %s\n", editors[0].filename);
        printf("  - %s\n", editors[1].filename);
    } else {
        printf("✓ 文件已保存並退出: %s\n", editors[0].filename);
    }
    printf("再見！\n\n");

    return 0;
}