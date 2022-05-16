#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char buf[1024];


int getFileDsc(char *name){
    int fd;
    if((fd = open(name, 0)) == -1){
        fprintf(2, "find: %s: No such file or directory\n", name);
        exit(1);
    }
    return fd;
}

int getFileType(int fd){
    struct stat st;

    if(fstat(fd, &st) < 0){
        fprintf(2, "find: cannot stat file");
        exit(1);
    }

    return st.type;
}


void find(char* file, char *p){
    struct dirent de;

    int fd = getFileDsc(buf);

    if(getFileType(fd) != T_DIR){
        fprintf(2, "find: %s: Not a directory\n", buf);
        exit(1);
    }


    while(read(fd, &de, sizeof(de)) == sizeof(de)) if(de.inum){
        if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) continue;

        strcpy(p, de.name);

        int __fd = getFileDsc(buf);
        int __type = getFileType(__fd);
        close(__fd);

        if(__type == T_FILE){
            if(strcmp(de.name, file) == 0) printf("%s\n", buf);
        }
        else if(__type == T_DIR){
            int length = strlen(p);
            
            p[length] = '/';
            p[length + 1] = '\0';
            
            find(file, p + length + 1);

            p[length] = '\0';
        }
    }

}

int main(int argc, char* argv[]){
    if(argc != 3){
        fprintf(2, "usage: find dir file\n");
        exit(1);
    }

    char *p = buf;
    strcpy(p, argv[1]);
    p += strlen(buf);
    *p++ = '/';

    find(argv[2], p);


    exit(0);
}