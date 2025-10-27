#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void edit_line(char *buffer,int current_line){
    for (int i=0;i<current_line;i++){
        buffer=strchr(buffer,'\n')+1;
    }
    
    char *line_end=strchr(buffer,'\n');
    char saved[1024]={0};
    strcpy(saved,line_end);
    scanf("%s",buffer);
    strcpy(buffer+strlen(buffer),saved);

}

int main(int argc, char **argv){
    FILE *file = fopen(argv[1],"r");
    char buffer[1024]={0};
    fread(buffer,1,1024,file);
    fclose(file);
    printf("Cotents:\n%s\n",buffer);

    int current_line=0;
    scanf("%d",&current_line);//input want to edit line
    edit_line(buffer,current_line);

    file=fopen(argv[1],"w");
    fwrite(buffer,strlen(buffer),1,file);
    fclose(file);

    return 0;
}