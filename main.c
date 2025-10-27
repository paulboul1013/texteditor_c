#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>

struct termios orig_termios;

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
            if (seq[1] == 'A') return 'U'; // Up arrow
            if (seq[1] == 'B') return 'D'; // Down arrow
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

// 顯示內容時帶行號
void print_with_line_numbers(char *buffer, int highlight_line){
    char *line_start = buffer;
    char *line_end;
    int line_num = 1;
    
    printf("\n========== 文件內容 ==========\n");
    while(*line_start){
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
    }
    printf("==============================\n\n");
}

void edit_line(char *buffer,int current_line){
    // 暫時關閉 raw mode 以便正常輸入
    disable_raw_mode();
    
    for (int i=0;i<current_line-1;i++){
        buffer=strchr(buffer,'\n')+1;
    }
    
    char *line_end=strchr(buffer,'\n');
    char saved[1024]={0};
    strcpy(saved,line_end);
    
    printf("\n>>> 編輯模式 - 行 %d <<<\n", current_line);
    printf("目前內容: ");
    char *temp = strchr(buffer, '\n');
    if(temp){
        printf("%.*s\n", (int)(temp - buffer), buffer);
    }
    printf("輸入新內容: ");
    scanf("%s",buffer);
    strcpy(buffer+strlen(buffer),saved);
    
    printf("\n✓ 已更新行 %d\n", current_line);
    printf("按任意鍵繼續...");
    
    // 重新啟用 raw mode
    enable_raw_mode();
    read_key();
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
    
    // 啟用原始模式來讀取方向鍵
    enable_raw_mode();
    
    clear_screen();
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║       文本編輯器 - 鍵盤導航模式          ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");
    printf("操作說明：\n");
    printf("  ↑/↓  - 上下移動選擇行\n");
    printf("  Enter - 編輯選中的行\n");
    printf("  q     - 退出編輯器\n\n");
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
        print_with_line_numbers(buffer, current_line);
        
        // 顯示提示信息
        printf("\n");
        printf("當前選擇：第 %d 行 (共 %d 行)\n", current_line, total_lines);
        printf("操作：[↑↓] 移動  [Enter] 編輯  [q] 退出\n");
        
        // 讀取按鍵
        char key = read_key();
        
        if(key == 'q' || key == 'Q'){
            // 退出
            clear_screen();
            disable_raw_mode();
            printf("\n正在退出編輯器...\n");
            break;
        }
        else if(key == 'U'){
            // 上移
            if(current_line > 1){
                current_line--;
            }
        }
        else if(key == 'D'){
            // 下移
            if(current_line < total_lines){
                current_line++;
            }
        }
        else if(key == '\r' || key == '\n'){
            // Enter - 編輯當前行
            clear_screen();
            print_with_line_numbers(buffer, current_line);
            
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