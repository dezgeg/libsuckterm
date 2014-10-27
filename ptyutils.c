#include "helpers.h"
#include "ptyutils.h"
#include <pty.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

pid_t child_pid;

void execsh(unsigned long windowid, char** cmd, char* shell, char* termname) {
    char** args;
    char* envshell = getenv("SHELL");
    const struct passwd* pass = getpwuid(getuid());
    char buf[sizeof(long) * 8 + 1];

    unsetenv("COLUMNS");
    unsetenv("LINES");
    unsetenv("TERMCAP");

    if (pass) {
        setenv("LOGNAME", pass->pw_name, 1);
        setenv("USER", pass->pw_name, 1);
        setenv("SHELL", pass->pw_shell, 0);
        setenv("HOME", pass->pw_dir, 0);
    }

    snprintf(buf, sizeof(buf), "%lu", windowid);
    setenv("WINDOWID", buf, 1);

    signal(SIGCHLD, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGALRM, SIG_DFL);

    DEFAULT(envshell, shell);
    setenv("TERM", termname, 1);
    args = cmd ? cmd : (char* []){ envshell, "-i", NULL };
    execvp(args[0], args);
    exit(EXIT_FAILURE);
}

void sigchld(int a) {
    int st = 0;

    if (waitpid(child_pid, &st, 0) < 0) {
        die("Waiting for pid %hd failed: %s\n", child_pid, SERRNO);
    }

    if (WIFEXITED(st)) {
        exit(WEXITSTATUS(st));
    } else {
        exit(EXIT_FAILURE);
    }
}

int ttynew(unsigned short row, unsigned short col, unsigned long windowid, char** cmd, char* shell, char* termname) {
    int m, s;
    struct winsize w = { row, col, 0, 0 };

    /* seems to work fine on linux, openbsd and freebsd */
    if (openpty(&m, &s, NULL, NULL, &w) < 0) {
        die("openpty failed: %s\n", SERRNO);
    }

    switch (child_pid = fork()) {
        case -1:
            die("fork failed\n");
            break;
        case 0:
            setsid(); /* create a new process group */
            dup2(s, STDIN_FILENO);
            dup2(s, STDOUT_FILENO);
            dup2(s, STDERR_FILENO);
            if (ioctl(s, TIOCSCTTY, NULL) < 0) {
                die("ioctl TIOCSCTTY failed: %s\n", SERRNO);
            }
            close(s);
            close(m);
            execsh(windowid, cmd, shell, termname);
            break;
        default:
            close(s);
            signal(SIGCHLD, sigchld);
            return m;
    }
    return -1;
}
