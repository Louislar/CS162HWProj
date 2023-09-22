#include <stdio.h>
#include <sys/resource.h>

int main() {
    struct rlimit stackLim;
    struct rlimit procLim;
    struct rlimit fileLim;
    if(
        getrlimit(RLIMIT_STACK, &stackLim) == 0 && 
        getrlimit(RLIMIT_NPROC, &procLim) == 0 &&
        getrlimit(RLIMIT_NOFILE, &fileLim) == 0
    ) {
        printf("stack size: %ld\n", stackLim.rlim_cur);
        printf("process limit: %ld\n", procLim.rlim_cur);
        printf("max file descriptors: %ld\n", fileLim.rlim_cur);
    }
    else {
        printf("Somthing went wrong with syscall \"getrlimit\" \n");
    }
        
    return 0;
}
