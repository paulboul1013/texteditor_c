#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>

struct termios orig_termios;

#define VISIBLE_LINES 15  // 一次顯示的行數

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
#define KEY_UP    1
#define KEY_DOWN  2
#define KEY_RIGHT 3
#define KEY_LEFT  4

// 讀取按鍵
char read_key() {
    char c;
    while (read(STDIN_FILENO, &c, 1) != 1);
    
    // 檢測方向鍵（ESC序列）
    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return c;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return c;
        
        if (seq[0] == '[') {
            if (seq[1] == 'A') return KEY_UP;      // Up arrow
            if (seq[1] == 'B') return KEY_DOWN;    // Down arrow
            if (seq[1] == 'C') return KEY_RIGHT;   // Right arrow
            if (seq[1] == 'D') return KEY_LEFT;    // Left arrow
        }
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
void print_with_line_numbers(char *buffer, int highlight_line, int row_offset, int total_lines){
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
            printf("%.*s", line_length, line_start);
            printf(" <<<\033[0m\n");  // 重置顏色
        } else {
            printf("    [行 %d] %.*s\n", line_num, line_length, line_start);
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

void edit_line(char *buffer, int current_line){
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
        printf("╚═══════════════════════════════════════════╝\n\n");
        printf("操作說明：\n");
        printf("  ←/→      - 左右移動光標\n");
        printf("  Backspace - 刪除光標前的字符\n");
        printf("  任意字符  - 在光標位置插入\n");
        printf("  Enter    - 完成編輯\n");
        printf("  ESC      - 取消編輯\n\n");
        
        printf("編輯內容：\n");
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
        // printf("\n光標位置：%d/%d\n", cursor_pos, content_len);
        
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

int main(int argc,char **argv){
    argc--;
    char *filename=argv[1];

    FILE *file = fopen(filename,"r");
    char buffer[1024]={0};
    fread(buffer,1,1024,file);
    fclose(file);
    
    // 計算總行數
    int total_lines = count_lines(buffer);
    if(total_lines == 0){
        printf("文件為空！\n");
        return 1;
    }
    
    int current_line = 1;  // 當前選中的行
    int row_offset = 1;        // 視窗頂部的行號
    
    // 啟用原始模式來讀取方向鍵
    enable_raw_mode();
    
    clear_screen();
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║       文本編輯器 - 鍵盤導航模式          ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");
    printf("操作說明：\n");
    printf("  ↑/↓   - 上下移動選擇行\n");
    printf("  Enter - 進入編輯模式\n");
    printf("  n     - 在當前行之後新增一行\n");
    printf("  d     - 刪除當前行\n");
    printf("  q     - 退出編輯器\n\n");
    printf("編輯模式功能：\n");
    printf("  ←/→      - 左右移動光標\n");
    printf("  字符輸入  - 在光標位置插入\n");
    printf("  Backspace - 刪除字符\n\n");
    printf("按任意鍵開始...\n");
    read_key();
    
    // 主循環
    while(1){
        clear_screen();
        
        // 顯示標題
        printf("╔═══════════════════════════════════════════╗\n");
        printf("║  文件: %-35s║\n", filename);
        printf("╚═══════════════════════════════════════════╝\n");
        
        // 顯示文件內容，高亮當前行
        print_with_line_numbers(buffer, current_line, row_offset, total_lines);
        
        // 顯示提示信息
        printf("\n");
        printf("當前選擇：第 %d 行 (共 %d 行)\n", current_line, total_lines);
        printf("操作：[↑↓] 移動  [Enter] 編輯  [n] 新增行  [d] 刪除行  [q] 退出\n");
        
        // 讀取按鍵
        char key = read_key();
        
        if(key == 'q' || key == 'Q'){
            // 退出
            clear_screen();
            disable_raw_mode();
            printf("\n正在退出編輯器...\n");
            break;
        }
        else if(key == KEY_UP){
            // 上移
            if(current_line > 1){
                current_line--;
                // 如果當前行移出視窗頂部，調整視窗
                if(current_line < row_offset){
                    row_offset = current_line;
                }
            }
        }
        else if(key == KEY_DOWN){
            // 下移
            if(current_line < total_lines){
                current_line++;
                // 如果當前行移出視窗底部，調整視窗
                if(current_line >= row_offset + VISIBLE_LINES){
                    row_offset = current_line - VISIBLE_LINES + 1;
                }
            }
        }
        else if(key == 'n' || key == 'N'){
            // 在當前行之後新增一行
            clear_screen();


            print_with_line_numbers(buffer, current_line, row_offset, total_lines);

            insert_new_line(buffer, current_line);
            
            // 自動保存
            file = fopen(filename, "w");
            fwrite(buffer, strlen(buffer), 1, file);
            fclose(file);
            
            // 重新計算行數
            total_lines = count_lines(buffer);

            // 移動到新插入的行
            current_line++;
            if(current_line > total_lines){
                current_line = total_lines;
            }
            // 調整視窗位置
            if(current_line >= row_offset + VISIBLE_LINES){
                row_offset = current_line - VISIBLE_LINES + 1;
            }
        }
        else if(key == 'd' || key == 'D'){
            // 刪除當前行
            clear_screen();
            print_with_line_numbers(buffer, current_line, row_offset, total_lines);
            
            // 嘗試刪除當前行
            int deleted = delete_line(buffer, current_line);
            
            if(deleted){
                // 自動保存
                file = fopen(filename, "w");
                fwrite(buffer, strlen(buffer), 1, file);
                fclose(file);
                
                // 重新計算行數
                total_lines = count_lines(buffer);
                
                // 調整當前行位置
                if(current_line > total_lines){
                    current_line = total_lines;
                }
                if(current_line < 1){
                    current_line = 1;
                }
                
                // 調整視窗位置
                if(current_line < row_offset){
                    row_offset = current_line;
                }
                if(current_line >= row_offset + VISIBLE_LINES){
                    row_offset = current_line - VISIBLE_LINES + 1;
                }
            }
        }
        else if(key == KEY_LEFT || key == KEY_RIGHT){
            // 左右方向鍵在主選單中不執行任何操作（僅在編輯模式中使用）
            // 忽略這些按鍵，避免未處理的輸入
        }
        else if(key == '\r' || key == '\n'){
            // Enter - 編輯當前行
            clear_screen();
            print_with_line_numbers(buffer, current_line, row_offset, total_lines);
            
            edit_line(buffer, current_line);
            
            // 自動保存
            file = fopen(filename, "w");
            fwrite(buffer, strlen(buffer), 1, file);
            fclose(file);
            
            // 重新計算行數（可能有變化）
            total_lines = count_lines(buffer);
            if(current_line > total_lines){
                current_line = total_lines;
            }
        }
    }
    
    // 最終保存
    file = fopen(filename, "w");
    fwrite(buffer, strlen(buffer), 1, file);
    fclose(file);
    
    printf("✓ 文件已保存並退出: %s\n", filename);
    printf("再見！\n\n");

    return 0;
}