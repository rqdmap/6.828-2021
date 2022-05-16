#include "kernel/types.h"
#include "user/user.h"


int main(){
    int p[2], pid;

    if(pipe(p)){
        fprintf(2, "Pipes creating failed.\n");
        exit(1);
    }

    if((pid = fork())){
        uint8 msg = 0x11;
        write(p[1], &msg, 1);

        read(p[0], &msg, 1);
        fprintf(2, "%d: received pong\n", getpid());
    }
    else{
        uint8 msg;
        read(p[0], &msg, 1);
        fprintf(2, "%d: received ping\n", getpid());


        msg = 0x22;
        write(p[1], &msg, 1);
    }


    exit(0);
}
