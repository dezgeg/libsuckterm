#include <sys/types.h>

extern pid_t child_pid;

void execsh(unsigned long windowid, char** cmd, char* shell, char* termname);
void sigchld(int a);
int ttynew(unsigned short row, unsigned short col, unsigned long windowid, char** cmd, char* shell, char* termname);
