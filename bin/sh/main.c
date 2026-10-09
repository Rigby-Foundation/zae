/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* sh [-eux...] [-c command [name [arg ...]] | file [arg ...] | -s [arg ...]]
 * At the console: a prompt with the directory (or $PS1), ^C for the
 * foreground job, "[exit N]" after a command that failed. */
#include "sh.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int sh_set_args(int argc, char **argv);
extern char **environ;

/* a line from standard input, a byte at a time: a script read from a pipe
 * must not swallow what the commands it runs are meant to read */
static int read_line(struct input *in)
{
    int got = 0;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r < 0 && errno == EINTR) {
            if (sh.interactive) { putchar('\n'); fflush(stdout); return got ? 0 : 1; }   /* ^C at the prompt */
            continue;
        }
        if (r < 0 && errno == EAGAIN) {         /* a program left the terminal non-blocking */
            fcntl(0, F_SETFL, fcntl(0, F_GETFL) & ~O_NONBLOCK);
            continue;
        }
        if (r <= 0) return got ? 0 : -1;
        input_append(in, &c, 1);
        got = 1;
        if (c == '\n') return 0;
    }
}

static void prompt(const char *which)
{
    if (!sh.interactive) return;
    const char *p = var_get(which);
    if (p) {
        char *e = expand_one(p, 0);
        fputs(e ? e : p, stderr);
        free(e);
    } else if (!strcmp(which, "PS1")) {
        char cwd[256];
        if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
        fprintf(stderr, "%s $ ", cwd);
    } else {
        fputs("> ", stderr);
    }
    fflush(stderr);
}

static int more(struct input *in)
{
    prompt("PS2");
    return read_line(in) == 0 ? 0 : -1;
}

/* commands from standard input (the console, or a pipe) until it ends */
static void run_stdin(void)
{
    struct input in;
    input_init_string(&in, "", 0);
    in.more = more;
    for (;;) {
        if (in.pos >= in.len) {                 /* a fresh command: forget the old text */
            if (sh.interactive) {
                int st;
                while (waitpid(-1, &st, WNOHANG) > 0) ;     /* finished background jobs */
            }
            in.len = in.pos = 0;
            in.eof = 0;
            prompt("PS1");
            int r = read_line(&in);
            if (r < 0) break;
            if (r > 0) continue;                /* ^C */
        }
        int err;
        struct node *t = parse_command(&in, &err);
        if (err) {
            sh.status = 2;
            if (!sh.interactive) sh_exit(2);
            in.pos = in.len;
            continue;
        }
        if (!t) {
            if (in.eof) break;
            continue;
        }
        if (sh.opt_n) continue;
        int st = exec_node(t);
        if (sh.interactive) {
            if (st > 128 && st < 128 + 65) printf("[killed by signal %d]\n", st - 128);
            else if (st) printf("[exit %d]\n", st);
            fflush(stdout);
        }
    }
    if (sh.interactive) putchar('\n');
}

int main(int argc, char **argv)
{
    sh.pid = getpid();
    vars_init(environ);
    var_set("IFS", " \t\n", 0);
    var_set("PS4", "+ ", 0);
    var_set("OPTIND", "1", 0);
    char num[32];
    snprintf(num, sizeof num, "%d", (int)getppid());
    var_set("PPID", num, 0);
    char cwd[1024];
    if (getcwd(cwd, sizeof cwd)) var_set("PWD", cwd, V_EXPORT);
    if (!var_get("PATH")) var_set("PATH", "/usr/bin:/bin", V_EXPORT);
    arg0 = argv[0];

    if (argc == 3 && !strcmp(argv[1], "--dump")) {          /* the parse tree of a file, for debugging */
        FILE *f = fopen(argv[2], "r");
        if (!f) { perror(argv[2]); return 1; }
        struct str b = { 0 };
        char buf[4096];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, f)) > 0) str_addn(&b, buf, r);
        fclose(f);
        struct input in;
        input_init_string(&in, b.s ? b.s : "", b.len);
        for (;;) {
            int err;
            struct node *t = parse_command(&in, &err);
            if (err) return 2;
            if (!t) break;
            dump_node(t, 0);
        }
        return 0;
    }

    /* options, then -c string / a file / the positional parameters */
    const char *cmd = NULL;
    int i = 1, force_i = 0, force_s = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--") || !strcmp(a, "-")) { i++; break; }
        if ((a[0] != '-' && a[0] != '+') || !a[1]) break;
        if (a[1] == 'o') {
            char *v[] = { "set", (char *)a, i + 1 < argc ? argv[i + 1] : NULL, NULL };
            if (sh_set_args(i + 1 < argc ? 3 : 2, v)) return 2;
            if (i + 1 < argc) i++;
            continue;
        }
        int bad = 0;
        for (const char *c = a + 1; *c && !bad; c++) {
            if (*c == 'c' && a[0] == '-') { cmd = ""; continue; }
            if (*c == 'i') { force_i = 1; continue; }
            if (*c == 's') { force_s = 1; continue; }
            char opt[3] = { a[0], *c, 0 };
            char *v[] = { "set", opt, NULL };
            if (sh_set_args(2, v)) bad = 1;
        }
        if (bad) return 2;
    }

    if (cmd) {
        if (i >= argc) { sh_error("-c: a command string is needed"); return 2; }
        cmd = argv[i++];
        if (i < argc) arg0 = argv[i++];
        params_set(argv + i, argc - i);
    } else if (i < argc && !force_s) {
        arg0 = argv[i++];
        params_set(argv + i, argc - i);
        sh.status = 0;
        int st = run_file(arg0, 1);
        if (st < 0) sh_exit(127);
        sh_exit(sh.status);
    } else {
        params_set(argv + i, argc - i);
    }

    if (cmd) {
        run_string(cmd, "-c");
        sh_exit(sh.status);
    }

    sh.interactive = force_i || (isatty(0) && isatty(2));
    if (sh.interactive) {
        signal(SIGINT, SIG_IGN);                /* ^C is for the foreground job */
        signal(SIGQUIT, SIG_IGN);
        tcsetpgrp(0, getpid());
        printf("sic shell (pid %d, musl libc). try: help\n", (int)getpid());
    }
    run_stdin();
    sh_exit(sh.status);
}
