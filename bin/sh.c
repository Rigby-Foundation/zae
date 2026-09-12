/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* A very small shell: builtins cd/pwd/exit/help/export, everything else is
 * fork + execvp (PATH lookup), waited for unless it ends in '&'. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define MAX_ARGS 32

static int split(char *line, char **argv)
{
    int argc = 0;
    char *save;
    for (char *tok = strtok_r(line, " \t\n", &save); tok && argc < MAX_ARGS - 1;
         tok = strtok_r(NULL, " \t\n", &save))
        argv[argc++] = tok;
    argv[argc] = NULL;
    return argc;
}

static void run(int argc, char **argv)
{
    int background = 0;
    if (argc && strcmp(argv[argc - 1], "&") == 0) {
        background = 1;
        argv[--argc] = NULL;
    }
    if (!argc)
        return;

    pid_t pid = fork();
    if (pid < 0) {
        perror("sh: fork");
        return;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "sh: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (background) {
        printf("[%d]\n", pid);
        return;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status))
        printf("[killed by signal %d]\n", WTERMSIG(status));
    else if (WEXITSTATUS(status))
        printf("[exit %d]\n", WEXITSTATUS(status));
}

int main(void)
{
    char line[512], cwd[256];
    char *args[MAX_ARGS];

    printf("sic shell (pid %d, musl libc). try: help\n", getpid());
    for (;;) {
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "?");
        printf("%s $ ", cwd);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;
        int n = split(line, args);
        if (!n)
            continue;
        if (strcmp(args[0], "exit") == 0)
            return n > 1 ? atoi(args[1]) : 0;
        if (strcmp(args[0], "cd") == 0) {
            const char *dir = n > 1 ? args[1] : getenv("HOME");
            if (chdir(dir ? dir : "/") != 0)
                fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
            continue;
        }
        if (strcmp(args[0], "pwd") == 0) { puts(cwd); continue; }
        if (strcmp(args[0], "export") == 0 && n > 1) {
            char *eq = strchr(args[1], '=');
            if (eq) { *eq = '\0'; setenv(args[1], eq + 1, 1); }
            continue;
        }
        if (strcmp(args[0], "help") == 0) {
            puts("builtins: cd pwd exit export help");
            puts("programs: everything in /bin (ls cat echo hello uname env crash test sh)");
            continue;
        }
        run(n, args);
    }
    return 0;
}
