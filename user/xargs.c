#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

char **buf;

int main(int argc, char **argv){
    if(argc < 2){
        fprintf(2, "usage: xargs command [arguments]\n");
        exit(1);
    }

    buf = (char**)malloc(MAXARG * sizeof(char*));
    for(int i = 0; i < MAXARG; i++) buf[i] = (char*)malloc((1024 + 1) * sizeof(char));


    for(int i = 2; i < argc; i++) strcpy(buf[2], argv[i]);


    while(gets(buf[argc], 1024) && strlen(buf[argc])){
        buf[argc][strlen(buf[argc]) - 1] = '\0';
        buf[argc + 1] = 0;
        if(!fork()){
            exec(argv[1], buf + 1);
            printf("%s %s %s\n", argv[1], buf[2], buf[3]);
            exit(0);
        }
        else{
            wait(0);
        }
    }


    exit(0);
}
