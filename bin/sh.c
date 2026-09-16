/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* A small shell: builtins cd/pwd/exit/export/help; pipelines with '|',
 * redirections '<' '>' '>>', background jobs with '&', ^C kills the
 * foreground job. Commands are found via PATH (execvp). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define MAX_ARGS 32
#define MAX_CMDS 8

struct cmd {
    char *argv[MAX_ARGS];
    int argc;
    char *in, *out;
    int append;
};

static int split(char *line, struct cmd *cmds, int *background)
{
    int n = 0;
    char *save;
    *background = 0;
    for (char *seg = strtok_r(line, "|", &save); seg && n < MAX_CMDS; seg = strtok_r(NULL, "|", &save)) {
        struct cmd *c = &cmds[n];
        memset(c, 0, sizeof(*c));
        char *ts;
        for (char *tok = strtok_r(seg, " \t\n", &ts); tok; tok = strtok_r(NULL, " \t\n", &ts)) {
            if (strcmp(tok, "<") == 0) { c->in = strtok_r(NULL, " \t\n", &ts); continue; }
            if (strcmp(tok, ">") == 0) { c->out = strtok_r(NULL, " \t\n", &ts); c->append = 0; continue; }
            if (strcmp(tok, ">>") == 0) { c->out = strtok_r(NULL, " \t\n", &ts); c->append = 1; continue; }
            if (strcmp(tok, "&") == 0) { *background = 1; continue; }
            if (c->argc < MAX_ARGS - 1)
                c->argv[c->argc++] = tok;
        }
        c->argv[c->argc] = NULL;
        if (c->argc)
            n++;
    }
    return n;
}

static void child_exec(struct cmd *c, int in_fd, int out_fd)
{
    signal(SIGINT, SIG_DFL);
    if (in_fd != 0) { dup2(in_fd, 0); close(in_fd); }
    if (out_fd != 1) { dup2(out_fd, 1); close(out_fd); }
    if (c->in) {
        int fd = open(c->in, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "sh: %s: %s\n", c->in, strerror(errno)); _exit(1); }
        dup2(fd, 0); close(fd);
    }
    if (c->out) {
        int fd = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { fprintf(stderr, "sh: %s: %s\n", c->out, strerror(errno)); _exit(1); }
        dup2(fd, 1); close(fd);
    }
    execvp(c->argv[0], c->argv);
    fprintf(stderr, "sh: %s: %s\n", c->argv[0], strerror(errno));
    _exit(127);
}

static void run(struct cmd *cmds, int n, int background)
{
    pid_t pids[MAX_CMDS];
    int in_fd = 0;
    for (int i = 0; i < n; i++) {
        int pipefd[2] = { -1, -1 };
        if (i + 1 < n && pipe(pipefd) != 0) { perror("sh: pipe"); return; }
        pid_t pid = fork();
        if (pid < 0) { perror("sh: fork"); return; }
        if (pid == 0) {
            if (pipefd[0] >= 0) close(pipefd[0]);
            child_exec(&cmds[i], in_fd, pipefd[1] >= 0 ? pipefd[1] : 1);
        }
        pids[i] = pid;
        if (in_fd != 0) close(in_fd);
        if (pipefd[1] >= 0) close(pipefd[1]);
        in_fd = pipefd[0];
    }
    if (background) {
        printf("[%d]\n", pids[n - 1]);
        return;
    }
    /* ^C goes to the last process of the pipeline; the shell ignores it itself. */
    tcsetpgrp(0, pids[n - 1]);
    int status = 0;
    for (int i = 0; i < n; i++) {
        int st;
        while (waitpid(pids[i], &st, 0) < 0 && errno == EINTR)
            ;
        if (i == n - 1) status = st;
    }
    tcsetpgrp(0, getpid());
    if (WIFSIGNALED(status))
        printf("[killed by signal %d]\n", WTERMSIG(status));
    else if (WEXITSTATUS(status))
        printf("[exit %d]\n", WEXITSTATUS(status));
}

int main(void)
{
    char line[512], cwd[256];
    struct cmd cmds[MAX_CMDS];

    signal(SIGINT, SIG_IGN);
    tcsetpgrp(0, getpid());
    printf("sic shell (pid %d, musl libc). try: help\n", getpid());
    for (;;) {
        /* reap finished background jobs */
        int st;
        while (waitpid(-1, &st, WNOHANG) > 0)
            ;
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "?");
        printf("%s $ ", cwd);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            if (errno == EINTR) { putchar('\n'); continue; }
            if (errno == EAGAIN) {          /* a child left our terminal non-blocking or raw */
                fcntl(0, F_SETFL, fcntl(0, F_GETFL) & ~O_NONBLOCK);
                clearerr(stdin);
                continue;
            }
            break;
        }
        int background;
        int n = split(line, cmds, &background);
        if (!n)
            continue;
        char **args = cmds[0].argv;
        if (n == 1) {
            if (strcmp(args[0], "exit") == 0)
                return cmds[0].argc > 1 ? atoi(args[1]) : 0;
            if (strcmp(args[0], "cd") == 0) {
                const char *dir = cmds[0].argc > 1 ? args[1] : getenv("HOME");
                if (chdir(dir ? dir : "/") != 0)
                    fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
                continue;
            }
            if (strcmp(args[0], "pwd") == 0) { puts(cwd); continue; }
            if (strcmp(args[0], "export") == 0 && cmds[0].argc > 1) {
                char *eq = strchr(args[1], '=');
                if (eq) { *eq = '\0'; setenv(args[1], eq + 1, 1); }
                continue;
            }
            if (strcmp(args[0], "help") == 0) {
                puts("builtins: cd pwd exit export help");
                puts("syntax:   cmd args | cmd2 ... [< in] [> out | >> out] [&]   ^C interrupts the foreground job");
                puts("programs: everything in /bin (ls cat echo grep wc sleep kill ...)");
                continue;
            }
        }
        run(cmds, n, background);
    }
    return 0;
}
