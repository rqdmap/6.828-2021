#include "kernel/types.h"
#include "user/user.h"

#define MAX_NUM 35
#define MAX_P 5
int p[MAX_P + 1][2];

int main(){
    
    for(int i = 2; i <= MAX_P; i++){
        if(!pipe(p[i])) continue;

        fprintf(2, "Pipe failed to create.");
        exit(1);
    }

    for(int i = 2; i <= MAX_NUM; i++) write(p[2][1], &i, sizeof(int));
    close(p[2][1]);



    for(int i = 2; i < MAX_P; i++){
        if(!fork()){
            int x;
            while(read(p[i][0], &x, sizeof(int))){
                // if(i == MAX_P - 1) fprintf(2, "%d\n", x);
                if(x == i || x % i) write(p[i + 1][1], &x, sizeof(int));
            }
            close(p[i + 1][1]);
            exit(0);
        }
        else{
            close(p[i + 1][1]);
        }
    }

    int x;
    while(read(p[MAX_P][0], &x, sizeof(int)))
        printf("prime %d\n", x);
    exit(0);
}