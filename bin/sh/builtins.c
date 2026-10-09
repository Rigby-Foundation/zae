/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The builtins: the special ones POSIX asks for (they change the shell
 * itself), and regular ones that scripts call often enough that a fork per
 * call would show (test, printf, echo, read, ...). */
#include "sh.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- signals by name ----------------------------------------------------------------- */

static const struct { const char *name; int num; } sigs[] = {
    { "EXIT", 0 }, { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "ILL", SIGILL }, { "TRAP", SIGTRAP },
    { "ABRT", SIGABRT }, { "BUS", SIGBUS }, { "FPE", SIGFPE }, { "KILL", SIGKILL }, { "USR1", SIGUSR1 }, { "SEGV", SIGSEGV },
    { "USR2", SIGUSR2 }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "CHLD", SIGCHLD }, { "CONT", SIGCONT },
    { "STOP", SIGSTOP }, { "TSTP", SIGTSTP }, { "TTIN", SIGTTIN }, { "TTOU", SIGTTOU }, { "WINCH", SIGWINCH },
};

int signal_number(const char *name)
{
    if (*name >= '0' && *name <= '9') { int n = atoi(name); return n >= 0 && n < 65 ? n : -1; }
    if (strncmp(name, "SIG", 3) == 0) name += 3;
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) if (strcmp(sigs[i].name, name) == 0) return sigs[i].num;
    return -1;
}

const char *signal_name(int sig)
{
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) if (sigs[i].num == sig) return sigs[i].name;
    return NULL;
}

/* ---- the special builtins ------------------------------------------------------------ */

static int bi_colon(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int bi_true(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int bi_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }

static int bi_exit(int argc, char **argv)
{
    int st = argc > 1 ? atoi(argv[1]) : sh.status;
    sh_exit(st & 0xff);
}

static int bi_return(int argc, char **argv)
{
    if (!sh.func_depth && !sh.source_depth) { sh_error("return: not in a function"); return 1; }
    sh.status = argc > 1 ? atoi(argv[1]) & 0xff : sh.status;
    sh.returning = 1;
    return sh.status;
}

static int loop_ctl(int argc, char **argv, int *counter)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 1) { sh_error("%s: bad count", argv[0]); return 1; }
    if (!sh.loop_depth) return 0;
    *counter = n > sh.loop_depth ? sh.loop_depth : n;
    return 0;
}
static int bi_break(int argc, char **argv) { return loop_ctl(argc, argv, &sh.breaking); }
static int bi_continue(int argc, char **argv) { return loop_ctl(argc, argv, &sh.continuing); }

static int bi_eval(int argc, char **argv)
{
    struct str b = { 0 };
    for (int i = 1; i < argc; i++) { if (i > 1) str_addc(&b, ' '); str_adds(&b, argv[i]); }
    char *s = str_take(&b);
    int st = run_string(s, "eval");
    free(s);
    return st;
}

static int bi_dot(int argc, char **argv)
{
    if (argc < 2) { sh_error(".: a file name is needed"); return 2; }
    char path[1024];
    const char *p = argv[1];
    if (!strchr(p, '/')) {
        if (find_in_path(p, path, sizeof path) == 0) p = path;
        else if (access(argv[1], R_OK) != 0) { sh_error("%s: not found", argv[1]); if (!sh.interactive) sh_exit(1); return 1; }
    }
    struct params *saved = NULL;
    if (argc > 2) saved = params_push(argv + 2, argc - 2);
    sh.source_depth++;
    int st = run_file(p, 1);
    sh.source_depth--;
    if (sh.returning) { sh.returning = 0; st = sh.status; }
    if (saved) params_pop(saved);
    if (st < 0) { if (!sh.interactive) sh_exit(1); return 1; }
    return st;
}

static int bi_shift(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 0 || n > params->n) { sh_error("shift: can't shift that many"); return 1; }
    params_set(params->v + n, params->n - n);
    return 0;
}

static int set_option(char c, int on)
{
    switch (c) {
    case 'e': sh.opt_e = on; return 0;
    case 'u': sh.opt_u = on; return 0;
    case 'x': sh.opt_x = on; return 0;
    case 'f': sh.opt_f = on; return 0;
    case 'n': sh.opt_n = on; return 0;
    case 'v': sh.opt_v = on; return 0;
    case 'C': sh.opt_C = on; return 0;
    case 'a': sh.opt_a = on; return 0;
    case 'm': case 'b': case 'h': return 0;     /* job control and friends: accepted, nothing to do */
    }
    return -1;
}

static int set_long_option(const char *name, int on)
{
    static const struct { const char *name; char c; } longs[] = {
        { "errexit", 'e' }, { "nounset", 'u' }, { "xtrace", 'x' }, { "noglob", 'f' }, { "noexec", 'n' },
        { "verbose", 'v' }, { "noclobber", 'C' }, { "allexport", 'a' }, { "monitor", 'm' }, { "notify", 'b' },
        { "hashall", 'h' }, { "pipefail", 'm' }, { "ignoreeof", 'm' }, { "vi", 'm' }, { "emacs", 'm' },
    };
    for (size_t i = 0; i < sizeof longs / sizeof longs[0]; i++)
        if (strcmp(longs[i].name, name) == 0) return set_option(longs[i].c, on);
    return -1;
}

int sh_set_args(int argc, char **argv)          /* also for the shell's own command line */
{
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; params_set(argv + i, argc - i); return 0; }
        if ((a[0] != '-' && a[0] != '+') || !a[1]) break;
        int on = a[0] == '-';
        if (a[1] == 'o') {
            if (i + 1 >= argc) {                /* set -o: the options as they are */
                printf("errexit %s\nnounset %s\nxtrace %s\nnoglob %s\nnoclobber %s\nallexport %s\n",
                       sh.opt_e ? "on" : "off", sh.opt_u ? "on" : "off", sh.opt_x ? "on" : "off",
                       sh.opt_f ? "on" : "off", sh.opt_C ? "on" : "off", sh.opt_a ? "on" : "off");
                return 0;
            }
            if (set_long_option(argv[++i], on) != 0) { sh_error("set: %s: unknown option", argv[i]); return 2; }
            continue;
        }
        for (const char *c = a + 1; *c; c++)
            if (set_option(*c, on) != 0) { sh_error("set: -%c: unknown option", *c); return 2; }
    }
    if (i < argc) params_set(argv + i, argc - i);
    return 0;
}

static int bi_set(int argc, char **argv)
{
    if (argc == 1) { var_print(0, 0); return 0; }
    return sh_set_args(argc, argv);
}

static int bi_export(int argc, char **argv)
{
    int ro = strcmp(argv[0], "readonly") == 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-p") == 0) i++;
    if (i >= argc) { var_print(!ro, ro); return 0; }
    int st = 0;
    for (; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        size_t nl = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!valid_name(argv[i], nl)) { sh_error("%s: %s: bad variable name", argv[0], argv[i]); st = 1; continue; }
        char *name = xstrndup(argv[i], nl);
        if (eq && var_set(name, eq + 1, 0) != 0) st = 1;
        if (ro) var_readonly(name);
        else var_export(name);
        free(name);
    }
    return st;
}

static int bi_unset(int argc, char **argv)
{
    int funcs = 0, i = 1, st = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-f")) funcs = 1;
        else if (!strcmp(argv[i], "-v")) funcs = 0;
        else if (!strcmp(argv[i], "--")) { i++; break; }
    }
    for (; i < argc; i++) {
        if (funcs) func_unset(argv[i]);
        else if (var_unset(argv[i]) != 0) st = 1;
    }
    return st;
}

static int bi_times(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("0m0.000s 0m0.000s\n0m0.000s 0m0.000s");
    return 0;
}

static int bi_trap(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "--") == 0) i++;
    if (i >= argc || (argc == 2 && strcmp(argv[1], "-p") == 0)) {
        for (int s = 0; s < 65; s++) {
            if (!sh.traps[s]) continue;
            const char *n = signal_name(s);
            printf("trap -- '%s' %s\n", sh.traps[s], n ? n : "?");
        }
        return 0;
    }
    const char *action = argv[i];
    int reset = 0;
    /* trap N...: all are conditions, to reset; trap - N...: the same */
    if (signal_number(action) >= 0 && (action[0] >= '0' && action[0] <= '9')) reset = 1;
    else { if (strcmp(action, "-") == 0) reset = 1; i++; }
    int st = 0;
    for (; i < argc; i++) {
        int s = signal_number(argv[i]);
        if (s < 0) { sh_error("trap: %s: bad signal", argv[i]); st = 1; continue; }
        install_trap(s, reset ? NULL : action);
    }
    return st;
}

/* ---- regular builtins ----------------------------------------------------------------- */

static int bi_cd(int argc, char **argv)
{
    int i = 1;
    while (i < argc && (!strcmp(argv[i], "-L") || !strcmp(argv[i], "-P"))) i++;
    const char *dir = i < argc ? argv[i] : var_get("HOME");
    int print = 0;
    if (!dir) dir = "/";
    if (strcmp(dir, "-") == 0) { dir = var_get("OLDPWD"); print = 1; if (!dir) { sh_error("cd: OLDPWD not set"); return 1; } }
    char old[PATH_MAX];
    if (!getcwd(old, sizeof old)) old[0] = 0;
    if (chdir(dir) != 0) { sh_error("cd: %s: %s", dir, strerror(errno)); return 1; }
    char now[PATH_MAX];
    if (getcwd(now, sizeof now)) { var_set("OLDPWD", old, 0); var_set("PWD", now, 0); if (print) puts(now); }
    return 0;
}

static int bi_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) { sh_error("pwd: %s", strerror(errno)); return 1; }
    puts(cwd);
    return 0;
}

/* the escapes of echo -e and printf %b / its format: returns 1 at \c (stop) */
static int put_escaped(const char *s, struct str *out)
{
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) { str_addc(out, *s); continue; }
        char c = *++s;
        switch (c) {
        case 'a': str_addc(out, '\a'); break;
        case 'b': str_addc(out, '\b'); break;
        case 'c': return 1;
        case 'e': str_addc(out, 27); break;
        case 'f': str_addc(out, '\f'); break;
        case 'n': str_addc(out, '\n'); break;
        case 'r': str_addc(out, '\r'); break;
        case 't': str_addc(out, '\t'); break;
        case 'v': str_addc(out, '\v'); break;
        case '\\': str_addc(out, '\\'); break;
        case '0': {                             /* \0nnn */
            int v = 0, k = 0;
            while (k < 3 && s[1] >= '0' && s[1] <= '7') { v = v * 8 + (*++s - '0'); k++; }
            str_addc(out, (char)v);
            break;
        }
        case 'x': {
            int v = 0, k = 0;
            while (k < 2 && ((s[1] >= '0' && s[1] <= '9') || ((s[1] | 32) >= 'a' && (s[1] | 32) <= 'f'))) {
                char h = *++s;
                v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                k++;
            }
            str_addc(out, (char)v);
            break;
        }
        default: str_addc(out, '\\'); str_addc(out, c);
        }
    }
    return 0;
}

static int bi_echo(int argc, char **argv)
{
    int i = 1, nl = 1, esc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i] + 1;
        if (strspn(o, "neE") != strlen(o)) break;
        for (; *o; o++) { if (*o == 'n') nl = 0; else if (*o == 'e') esc = 1; else esc = 0; }
    }
    struct str out = { 0 };
    int stop = 0;
    for (; i < argc && !stop; i++) {
        if (esc) stop = put_escaped(argv[i], &out);
        else str_adds(&out, argv[i]);
        if (i + 1 < argc && !stop) str_addc(&out, ' ');
    }
    if (nl && !stop) str_addc(&out, '\n');
    fflush(stdout);
    int st = 0;
    if (out.len && write(1, out.s, out.len) < 0) st = 1;
    free(out.s);
    return st;
}

/* read [-r] [-p prompt] [name ...]: a line from standard input, a byte at a
 * time so nothing after it is taken from a pipe */
static int bi_read(int argc, char **argv)
{
    int raw = 0, i = 1;
    const char *prompt = NULL;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r') raw = 1;
            else if (*o == 'p' && i + 1 < argc) { prompt = argv[++i]; break; }
            else if (*o == 's') {}
            else { sh_error("read: -%c: unknown option", *o); return 2; }
        }
    }
    if (prompt && isatty(0)) { fputs(prompt, stderr); fflush(stderr); }
    struct str line = { 0 }, mask = { 0 };      /* mask: 1 for characters escaped by \ (never separators) */
    int got = 0, eof = 0;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { eof = 1; break; }
        got = 1;
        if (c == '\n') break;
        if (c == '\\' && !raw) {
            char d;
            if (read(0, &d, 1) <= 0) { eof = 1; break; }
            if (d == '\n') continue;            /* a continued line */
            str_addc(&line, d); str_addc(&mask, 1);
            continue;
        }
        str_addc(&line, c); str_addc(&mask, 0);
    }
    const char *ifs = var_get("IFS");
    if (!ifs) ifs = " \t\n";
    char **names = argv + i;
    int nn = argc - i;
    const char *def[] = { "REPLY" };
    if (nn == 0) { names = (char **)def; nn = 1; }
    const char *s = line.s ? line.s : "";
    const char *m = mask.s ? mask.s : "";
    size_t len = line.len, p = 0;
    #define SEP(k) (!m[k] && strchr(ifs, s[k]) && s[k])
    #define WHITE(k) (SEP(k) && (s[k] == ' ' || s[k] == '\t' || s[k] == '\n'))
    while (p < len && WHITE(p)) p++;
    for (int k = 0; k < nn; k++) {
        size_t start = p, end;
        if (k == nn - 1) {
            end = len;
            while (end > start && WHITE(end - 1)) end--;
        } else {
            while (p < len && !SEP(p)) p++;
            end = p;
            while (p < len && WHITE(p)) p++;
            if (p < len && SEP(p)) { p++; while (p < len && WHITE(p)) p++; }
        }
        char *v = xstrndup(s + start, end - start);
        var_set(names[k], v, 0);
        free(v);
    }
    #undef SEP
    #undef WHITE
    free(line.s);
    free(mask.s);
    (void)got;
    return eof ? 1 : 0;                         /* the end of input: what was there is set, but it fails */
}

static int bi_local(int argc, char **argv)
{
    if (!sh.func_depth) { sh_error("local: not in a function"); return 1; }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        char *name = eq ? xstrndup(argv[i], (size_t)(eq - argv[i])) : xstrdup(argv[i]);
        var_local(name);
        if (eq) var_set(name, eq + 1, 0);
        free(name);
    }
    return 0;
}

static int optoff;                              /* getopts: where in the current argument */

static int bi_getopts(int argc, char **argv)
{
    if (argc < 3) { sh_error("getopts: usage: getopts optstring name [arg ...]"); return 2; }
    const char *opts = argv[1], *name = argv[2];
    char **args = argc > 3 ? argv + 3 : params->v;
    int nargs = argc > 3 ? argc - 3 : params->n;
    const char *oi = var_get("OPTIND");
    int ind = oi ? atoi(oi) : 1;
    if (ind < 1) { ind = 1; optoff = 0; }
    static int last_ind = 1;
    if (ind != last_ind) optoff = 0;
    int silent = opts[0] == ':';
    char num[16];
    for (;;) {
        if (ind > nargs) break;
        const char *a = args[ind - 1];
        if (optoff == 0) {
            if (a[0] != '-' || !a[1]) break;
            if (!strcmp(a, "--")) { ind++; break; }
            optoff = 1;
        }
        char c = a[optoff++];
        const char *spec = strchr(opts + silent, c);
        char opt[2] = { c, 0 };
        int done_arg = !a[optoff];
        if (!spec || c == ':') {
            if (done_arg) { ind++; optoff = 0; }
            if (silent) { var_set("OPTARG", opt, 0); var_set(name, "?", 0); }
            else { sh_error("illegal option -- %c", c); var_unset("OPTARG"); var_set(name, "?", 0); }
            goto out;
        }
        if (spec[1] == ':') {
            if (!done_arg) { var_set("OPTARG", a + optoff, 0); ind++; optoff = 0; }
            else if (ind < nargs) { var_set("OPTARG", args[ind], 0); ind += 2; optoff = 0; }
            else {
                ind++; optoff = 0;
                if (silent) { var_set("OPTARG", opt, 0); var_set(name, ":", 0); }
                else { sh_error("option requires an argument -- %c", c); var_unset("OPTARG"); var_set(name, "?", 0); }
                goto out;
            }
        } else {
            var_unset("OPTARG");
            if (done_arg) { ind++; optoff = 0; }
        }
        var_set(name, opt, 0);
        goto out;
    }
    /* the end of the options */
    optoff = 0;
    snprintf(num, sizeof num, "%d", ind);
    var_set("OPTIND", num, 0);
    last_ind = ind;
    var_set(name, "?", 0);
    return 1;
out:
    snprintf(num, sizeof num, "%d", ind);
    var_set("OPTIND", num, 0);
    last_ind = ind;
    return 0;
}

/* what a name is: 1 special builtin, 2 builtin, 3 function, 4 program (path in out), 0 none */
static int what_is(const char *name, char *out, size_t outlen)
{
    int special;
    if (builtin_find(name, &special) && special) return 1;
    if (func_get(name)) return 3;
    if (builtin_find(name, &special)) return 2;
    if (find_in_path(name, out, outlen) == 0) return 4;
    return 0;
}

static int describe(const char *name, int verbose)
{
    char path[1024];
    static const char *const kw[] = { "if", "then", "else", "elif", "fi", "do", "done", "case", "esac", "while", "until", "for", "{", "}", "!", "in", NULL };
    for (int i = 0; kw[i]; i++)
        if (!strcmp(kw[i], name)) { if (verbose) printf("%s is a shell keyword\n", name); else puts(name); return 0; }
    switch (what_is(name, path, sizeof path)) {
    case 1: if (verbose) printf("%s is a special shell builtin\n", name); else puts(name); return 0;
    case 2: if (verbose) printf("%s is a shell builtin\n", name); else puts(name); return 0;
    case 3: if (verbose) printf("%s is a function\n", name); else puts(name); return 0;
    case 4: if (verbose) printf("%s is %s\n", name, path); else puts(path); return 0;
    }
    if (verbose) sh_error("%s: not found", name);
    return 1;
}

static int bi_command(int argc, char **argv)
{
    int i = 1, v = 0, V = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *o = argv[i] + 1; *o; o++) { if (*o == 'v') v = 1; else if (*o == 'V') V = 1; else if (*o != 'p') { sh_error("command: -%c: unknown option", *o); return 2; } }
    }
    if (i >= argc) return 0;
    if (v || V) {
        int st = 0;
        for (; i < argc; i++) if (describe(argv[i], V)) st = 1;
        return st;
    }
    return run_argv(argc - i, argv + i);
}

static int bi_type(int argc, char **argv)
{
    int st = 0;
    for (int i = 1; i < argc; i++) if (describe(argv[i], 1)) st = 1;
    return st;
}

static int bi_wait(int argc, char **argv)
{
    int st = 0, s;
    if (argc < 2) {
        while (waitpid(-1, &s, 0) > 0 || errno == EINTR) ;
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        pid_t pid = (pid_t)atoi(argv[i]);
        for (;;) {
            pid_t r = waitpid(pid, &s, 0);
            if (r == pid) { st = WIFEXITED(s) ? WEXITSTATUS(s) : 128 + WTERMSIG(s); break; }
            if (r < 0 && errno != EINTR) { st = 127; break; }
        }
    }
    return st;
}

static int bi_umask(int argc, char **argv)
{
    if (argc < 2) {
        mode_t m = umask(0);
        umask(m);
        printf("%04o\n", (unsigned)m);
        return 0;
    }
    char *end;
    long m = strtol(argv[1], &end, 8);
    if (*end) { sh_error("umask: %s: only octal masks", argv[1]); return 1; }
    umask((mode_t)m);
    return 0;
}

static int bi_ulimit(int argc, char **argv) { (void)argc; (void)argv; puts("unlimited"); return 0; }
static int bi_hash(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int bi_alias(int argc, char **argv) { (void)argc; (void)argv; return 0; }   /* aliases aren't expanded here */

static int bi_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("sic's shell: the POSIX shell language (if/while/for/case, functions, $(...), here-documents, ...)");
    puts("builtins: . : break cd command continue echo eval exec exit export false getopts help local printf pwd");
    puts("          read readonly return set shift test [ times trap true type ulimit umask unset wait");
    puts("^C interrupts the foreground job; programs are found along $PATH (/usr/bin, then /bin)");
    return 0;
}

static int bi_bracket(int argc, char **argv)
{
    if (strcmp(argv[argc - 1], "]") != 0) { sh_error("[: missing ]"); return 2; }
    return builtin_test(argc - 1, argv);
}

/* ---- the table ------------------------------------------------------------------------ */

static const struct { const char *name; builtin_fn fn; int special; } table[] = {
    { ":", bi_colon, 1 }, { ".", bi_dot, 1 }, { "break", bi_break, 1 }, { "continue", bi_continue, 1 },
    { "eval", bi_eval, 1 }, { "exec", bi_colon, 1 }, { "exit", bi_exit, 1 }, { "export", bi_export, 1 },
    { "readonly", bi_export, 1 }, { "return", bi_return, 1 }, { "set", bi_set, 1 }, { "shift", bi_shift, 1 },
    { "times", bi_times, 1 }, { "trap", bi_trap, 1 }, { "unset", bi_unset, 1 },
    { "true", bi_true, 0 }, { "false", bi_false, 0 }, { "cd", bi_cd, 0 }, { "pwd", bi_pwd, 0 },
    { "echo", bi_echo, 0 }, { "printf", builtin_printf, 0 }, { "test", builtin_test, 0 }, { "[", bi_bracket, 0 },
    { "read", bi_read, 0 }, { "local", bi_local, 0 }, { "getopts", bi_getopts, 0 }, { "command", bi_command, 0 },
    { "type", bi_type, 0 }, { "wait", bi_wait, 0 }, { "umask", bi_umask, 0 }, { "ulimit", bi_ulimit, 0 },
    { "hash", bi_hash, 0 }, { "alias", bi_alias, 0 }, { "unalias", bi_alias, 0 }, { "help", bi_help, 0 },
};

builtin_fn builtin_find(const char *name, int *special)
{
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(table[i].name, name) == 0) { *special = table[i].special; return table[i].fn; }
    *special = 0;
    return NULL;
}

/* ---- test ------------------------------------------------------------------------------- */

struct tst { char **v; int n, i, err; };

static int t_or(struct tst *t);

static int file_test(char op, const char *path)
{
    struct stat st;
    if (op == 'h' || op == 'L') return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
    if (op == 't') return isatty(atoi(path));
    if (stat(path, &st) != 0) return 0;
    switch (op) {
    case 'e': return 1;
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'b': return S_ISBLK(st.st_mode);
    case 'p': return S_ISFIFO(st.st_mode);
    case 'S': return S_ISSOCK(st.st_mode);
    case 's': return st.st_size > 0;
    case 'r': return access(path, R_OK) == 0;
    case 'w': return access(path, W_OK) == 0;
    case 'x': return access(path, X_OK) == 0;
    case 'u': return (st.st_mode & S_ISUID) != 0;
    case 'g': return (st.st_mode & S_ISGID) != 0;
    case 'k': return (st.st_mode & S_ISVTX) != 0;
    case 'O': case 'G': return 1;
    }
    return 0;
}

static long t_num(struct tst *t, const char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    long v = strtol(s, &end, 10);
    while (*end == ' ' || *end == '\t') end++;
    if (*end || end == s) { sh_error("test: %s: integer expected", s); t->err = 1; }
    return v;
}

static int is_unary(const char *s) { return s[0] == '-' && s[1] && !s[2] && strchr("bcdefghkLprsStuwxzOGn", s[1]); }

static const char *const binops[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot", "-ef", NULL };
static int is_binary(const char *s) { for (int i = 0; binops[i]; i++) if (!strcmp(s, binops[i])) return 1; return 0; }

static int t_primary(struct tst *t)
{
    if (t->i >= t->n) { t->err = 1; return 0; }
    char *a = t->v[t->i];
    if (!strcmp(a, "(") ) {
        t->i++;
        int r = t_or(t);
        if (t->i >= t->n || strcmp(t->v[t->i], ")")) { sh_error("test: missing )"); t->err = 1; return 0; }
        t->i++;
        return r;
    }
    if (t->n - t->i >= 3 && is_binary(t->v[t->i + 1])) goto binary;
    if (is_unary(a) && t->i + 1 < t->n) {
        char op = a[1];
        const char *arg = t->v[t->i + 1];
        t->i += 2;
        if (op == 'z') return arg[0] == 0;
        if (op == 'n') return arg[0] != 0;
        return file_test(op, arg);
    }
    t->i++;
    return a[0] != 0;
binary: {
        const char *l = t->v[t->i], *op = t->v[t->i + 1], *r = t->v[t->i + 2];
        t->i += 3;
        if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(l, r);
        if (!strcmp(op, "!=")) return strcmp(l, r) != 0;
        if (!strcmp(op, "<")) return strcmp(l, r) < 0;
        if (!strcmp(op, ">")) return strcmp(l, r) > 0;
        if (!strcmp(op, "-nt") || !strcmp(op, "-ot") || !strcmp(op, "-ef")) {
            struct stat a1, b1;
            int ha = stat(l, &a1) == 0, hb = stat(r, &b1) == 0;
            if (!strcmp(op, "-ef")) return ha && hb && a1.st_ino == b1.st_ino && a1.st_dev == b1.st_dev;
            if (!strcmp(op, "-nt")) return ha && (!hb || a1.st_mtime > b1.st_mtime);
            return hb && (!ha || a1.st_mtime < b1.st_mtime);
        }
        long x = t_num(t, l), y = t_num(t, r);
        if (!strcmp(op, "-eq")) return x == y;
        if (!strcmp(op, "-ne")) return x != y;
        if (!strcmp(op, "-lt")) return x < y;
        if (!strcmp(op, "-le")) return x <= y;
        if (!strcmp(op, "-gt")) return x > y;
        return x >= y;
    }
}

static int t_not(struct tst *t)
{
    if (t->i < t->n && !strcmp(t->v[t->i], "!") && t->i + 1 < t->n) { t->i++; return !t_not(t); }
    return t_primary(t);
}

static int t_and(struct tst *t)
{
    int r = t_not(t);
    while (t->i < t->n && !strcmp(t->v[t->i], "-a")) { t->i++; int s = t_not(t); r = r && s; }
    return r;
}

static int t_or(struct tst *t)
{
    int r = t_and(t);
    while (t->i < t->n && !strcmp(t->v[t->i], "-o")) { t->i++; int s = t_and(t); r = r || s; }
    return r;
}

int builtin_test(int argc, char **argv)
{
    struct tst t = { argv + 1, argc - 1, 0, 0 };
    if (t.n == 0) return 1;
    /* POSIX's rules by argument count settle the ambiguous short forms */
    if (t.n == 1) return argv[1][0] == 0;
    if (t.n == 2 && !strcmp(argv[1], "!")) return argv[2][0] != 0;
    if (t.n == 3 && is_binary(argv[2])) { int r = t_primary(&t); return t.err ? 2 : !r; }
    if (t.n == 3 && !strcmp(argv[1], "!")) { t.i = 1; int r = t_primary(&t); return t.err ? 2 : r; }
    int r = t_or(&t);
    if (!t.err && t.i < t.n) { sh_error("test: %s: unexpected", t.v[t.i]); return 2; }
    return t.err ? 2 : !r;
}

/* ---- printf ------------------------------------------------------------------------------ */

static long long pf_num(const char *s, int *bad)
{
    if (!s) return 0;
    if (s[0] == '\'' || s[0] == '"') return (unsigned char)s[1];     /* 'c: its code */
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 0);
    if (*end || end == s) { if (*s) { sh_error("printf: %s: invalid number", s); *bad = 1; } }
    return v;
}

int builtin_printf(int argc, char **argv)
{
    if (argc < 2) { sh_error("printf: usage: printf format [arguments]"); return 2; }
    const char *fmt = argv[1];
    char **args = argv + 2;
    int nargs = argc - 2, ai = 0, bad = 0;
    struct str out = { 0 };
    do {
        int used_arg = 0;
        for (const char *f = fmt; *f; f++) {
            if (*f == '\\') {
                char tmp[3] = { '\\', f[1], 0 };
                if (!f[1]) { str_addc(&out, '\\'); break; }
                if (f[1] >= '0' && f[1] <= '7') {   /* \nnn */
                    int v = 0, k = 0;
                    while (k < 3 && f[1] >= '0' && f[1] <= '7') { v = v * 8 + (*++f - '0'); k++; }
                    str_addc(&out, (char)v);
                    continue;
                }
                f++;
                if (put_escaped(tmp, &out)) goto done;
                continue;
            }
            if (*f != '%') { str_addc(&out, *f); continue; }
            if (f[1] == '%') { str_addc(&out, '%'); f++; continue; }
            /* %[flags][width][.prec]conv */
            char spec[64];
            size_t sl = 0;
            spec[sl++] = '%';
            f++;
            while (*f && strchr("-+ #0", *f) && sl < 40) spec[sl++] = *f++;
            if (*f == '*') { long long w = pf_num(ai < nargs ? args[ai++] : NULL, &bad); used_arg = 1; sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%lld", w); f++; }
            else while (*f >= '0' && *f <= '9' && sl < 50) spec[sl++] = *f++;
            if (*f == '.') {
                spec[sl++] = *f++;
                if (*f == '*') { long long p = pf_num(ai < nargs ? args[ai++] : NULL, &bad); used_arg = 1; sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%lld", p); f++; }
                else while (*f >= '0' && *f <= '9' && sl < 58) spec[sl++] = *f++;
            }
            char conv = *f;
            if (!conv) break;
            const char *arg = ai < nargs ? args[ai] : NULL;
            if (strchr("diouxXcsb", conv) == NULL && strchr("eEfgG", conv) == NULL) { sh_error("printf: %%%c: invalid conversion", conv); bad = 1; continue; }
            ai++;
            used_arg = 1;
            char buf[512];
            int n = 0;
            if (conv == 's' || conv == 'b') {
                const char *s = arg ? arg : "";
                struct str esc = { 0 };
                int stop = 0;
                if (conv == 'b') { stop = put_escaped(s, &esc); s = esc.s ? esc.s : ""; }
                spec[sl++] = 's'; spec[sl] = 0;
                size_t need = strlen(s) + 64;
                char *big = xmalloc(need + 512);
                n = snprintf(big, need + 512, spec, s);
                if (n > 0) str_addn(&out, big, (size_t)n);
                free(big);
                free(esc.s);
                if (stop) goto done;
                continue;
            }
            if (conv == 'c') { spec[sl++] = 'c'; spec[sl] = 0; n = snprintf(buf, sizeof buf, spec, arg ? arg[0] : 0); }
            else if (strchr("eEfgG", conv)) { spec[sl++] = conv; spec[sl] = 0; n = snprintf(buf, sizeof buf, spec, arg ? strtod(arg, NULL) : 0.0); }
            else {
                spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
                long long v = pf_num(arg, &bad);
                n = snprintf(buf, sizeof buf, spec, v);
            }
            if (n > 0) str_addn(&out, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
        }
        if (!used_arg) break;
    } while (ai < nargs);
done:
    fflush(stdout);
    if (out.len && write(1, out.s, out.len) < 0) bad = 1;
    free(out.s);
    return bad;
}
