/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Running the tree (XCU 2.9): simple commands (special builtins, functions,
 * builtins, programs along PATH), pipelines, lists, the compound commands,
 * redirections (put back afterwards unless `exec` made them), subshells,
 * $(...), traps, set -e and -x. */
#include "sh.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct shell sh;
static unsigned subst_runs;     /* $(...) run so far: a command of assignments only exits with the last one's status */

#define SAVE_FD_MIN 10          /* where descriptors are parked while a redirection is in force */

/* ---- exit, signals, traps ----------------------------------------------------------- */

void sh_exit(int status)
{
    static int exiting;
    if (!exiting && sh.traps[0] && *sh.traps[0] && !sh.subshell) {
        exiting = 1;
        char *t = xstrdup(sh.traps[0]);
        sh.status = status;
        run_string(t, "trap");
        free(t);
    }
    fflush(NULL);
    exit(status);
}

static void on_signal(int sig)
{
    if (sig > 0 && sig < 65) { sh.pending_sig[sig] = 1; sh.any_pending = 1; }
}

void install_trap(int sig, const char *action)
{
    if (sig < 0 || sig > 64) return;
    free(sh.traps[sig]);
    sh.traps[sig] = action ? xstrdup(action) : NULL;
    if (sig == 0) return;
    if (!action) signal(sig, (sig == SIGINT && sh.interactive && !sh.subshell) ? SIG_IGN : SIG_DFL);
    else if (!*action) signal(sig, SIG_IGN);
    else signal(sig, on_signal);
}

void run_traps(void)
{
    if (!sh.any_pending) return;
    sh.any_pending = 0;
    int saved = sh.status;
    for (int s = 1; s < 65; s++) {
        if (!sh.pending_sig[s]) continue;
        sh.pending_sig[s] = 0;
        if (sh.traps[s] && *sh.traps[s]) {
            char *t = xstrdup(sh.traps[s]);
            run_string(t, "trap");
            free(t);
        }
    }
    sh.status = saved;
}

/* A forked copy of the shell: traps that run commands go back to the
 * default (ignored signals stay ignored), ^C kills it again. */
static void child_setup(void)
{
    sh.subshell = 1;
    for (int s = 1; s < 65; s++)
        if (sh.traps[s] && *sh.traps[s]) { free(sh.traps[s]); sh.traps[s] = NULL; signal(s, SIG_DFL); }
    if (!(sh.traps[SIGINT] && !*sh.traps[SIGINT])) signal(SIGINT, SIG_DFL);
    if (!(sh.traps[SIGQUIT] && !*sh.traps[SIGQUIT])) signal(SIGQUIT, SIG_DFL);
    free(sh.traps[0]);
    sh.traps[0] = NULL;
    sh.interactive = 0;
}

__attribute__((noreturn)) static void child_exit(int status)
{
    fflush(NULL);
    _exit(status & 0xff);
}

static int wait_status(int st)
{
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}

static int wait_for(pid_t pid)
{
    int st;
    for (;;) {
        pid_t r = waitpid(pid, &st, 0);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return 127;
    }
    run_traps();
    return wait_status(st);
}

/* ---- redirections -------------------------------------------------------------------- */

struct saved_fd { int fd, copy; };              /* copy -1: fd was closed before */
struct saved { struct saved_fd v[32]; int n; };

static pid_t heredoc_writers[16];
static int nheredoc_writers;

static void reap_heredoc_writers(void)
{
    for (int i = 0; i < nheredoc_writers;) {
        if (waitpid(heredoc_writers[i], NULL, WNOHANG) != 0) heredoc_writers[i] = heredoc_writers[--nheredoc_writers];
        else i++;
    }
}

static void save_fd(struct saved *sv, int fd)
{
    if (!sv || sv->n == 32) return;
    for (int i = 0; i < sv->n; i++) if (sv->v[i].fd == fd) return;
    int copy = fcntl(fd, F_DUPFD, SAVE_FD_MIN);
    if (copy >= 0) fcntl(copy, F_SETFD, FD_CLOEXEC);
    sv->v[sv->n].fd = fd;
    sv->v[sv->n].copy = copy;
    sv->n++;
}

static void restore_fds(struct saved *sv)
{
    for (int i = sv->n - 1; i >= 0; i--) {
        if (sv->v[i].copy >= 0) { dup2(sv->v[i].copy, sv->v[i].fd); close(sv->v[i].copy); }
        else close(sv->v[i].fd);
    }
    sv->n = 0;
}

static int heredoc_fd(struct redir *r)
{
    char *body = r->heredoc_quoted ? xstrdup(r->word) : NULL;
    if (!body) {
        struct fields f = { 0 };
        if (expand_word(r->word, EX_HEREDOC, &f) != 0) { fields_free(&f); return -1; }
        struct str b = { 0 };
        for (int i = 0; i < f.n; i++) { if (i) str_addc(&b, ' '); str_adds(&b, f.v[i]); }
        fields_free(&f);
        body = str_take(&b);
    }
    int p[2];
    if (pipe(p) != 0) { free(body); sh_error("pipe: %s", strerror(errno)); return -1; }
    size_t len = strlen(body);
    if (len < 60 * 1024) {                      /* fits in the pipe: no writer needed */
        if (len) (void)!write(p[1], body, len);
        close(p[1]);
    } else {
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            size_t off = 0;
            while (off < len) { ssize_t w = write(p[1], body + off, len - off); if (w <= 0) break; off += (size_t)w; }
            _exit(0);
        }
        close(p[1]);
        if (pid > 0 && nheredoc_writers < 16) heredoc_writers[nheredoc_writers++] = pid;
    }
    free(body);
    return p[0];
}

/* Apply a list of redirections; with sv, what they replace is kept to be put back. */
static int apply_redirs(struct redir *r, struct saved *sv)
{
    for (; r; r = r->next) {
        int fd = r->fd, nfd = -1;
        if (r->type == R_HEREDOC) {
            nfd = heredoc_fd(r);
            if (nfd < 0) return -1;
        } else {
            char *w = expand_one(r->word, EX_ASSIGN);
            if (!w) return -1;
            if (r->type == R_DUPIN || r->type == R_DUPOUT) {
                if (strcmp(w, "-") == 0) { save_fd(sv, fd); close(fd); free(w); continue; }
                char *end;
                long src = strtol(w, &end, 10);
                if (*end || end == w) { sh_error("%s: bad file descriptor", w); free(w); return -1; }
                if (fcntl((int)src, F_GETFD) < 0) { sh_error("%ld: %s", src, strerror(errno)); free(w); return -1; }
                free(w);
                if (src == fd) continue;
                save_fd(sv, fd);
                if (dup2((int)src, fd) < 0) { sh_error("dup2: %s", strerror(errno)); return -1; }
                continue;
            }
            int flags = 0;
            switch (r->type) {
            case R_IN: flags = O_RDONLY; break;
            case R_OUT: case R_CLOBBER: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
            case R_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
            case R_RDWR: flags = O_RDWR | O_CREAT; break;
            default: break;
            }
            if (r->type == R_OUT && sh.opt_C) {
                struct stat st;
                if (stat(w, &st) == 0 && S_ISREG(st.st_mode)) { sh_error("%s: cannot overwrite existing file", w); free(w); return -1; }
            }
            nfd = open(w, flags, 0666);
            if (nfd < 0) { sh_error("%s: %s", w, strerror(errno)); free(w); return -1; }
            free(w);
        }
        if (nfd != fd) {
            save_fd(sv, fd);
            dup2(nfd, fd);
            close(nfd);
        }
    }
    return 0;
}

/* ---- finding commands ---------------------------------------------------------------- */

int find_in_path(const char *name, char *out, size_t outlen)
{
    struct stat st;
    if (strchr(name, '/')) {
        if (strlen(name) >= outlen) return -1;
        strcpy(out, name);
        return stat(name, &st) == 0 ? 0 : -1;
    }
    const char *path = var_get("PATH");
    if (!path) path = "/usr/bin:/bin";
    for (const char *p = path;;) {
        const char *e = strchr(p, ':');
        size_t dl = e ? (size_t)(e - p) : strlen(p);
        if (dl + strlen(name) + 2 <= outlen) {
            if (dl) { memcpy(out, p, dl); out[dl] = '/'; strcpy(out + dl + 1, name); }
            else strcpy(out, name);
            if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0) return 0;
        }
        if (!e) break;
        p = e + 1;
    }
    return -1;
}

/* in a child: become the program (a file without #! is run as a script by sh) */
__attribute__((noreturn)) static void exec_program(char **argv)
{
    char path[1024];
    if (find_in_path(argv[0], path, sizeof path) != 0) {
        sh_error("%s: not found", argv[0]);
        child_exit(127);
    }
    char **env = var_environ();
    execve(path, argv, env);
    int e = errno;
    if (e == ENOEXEC) {
        int n = 0;
        while (argv[n]) n++;
        char **v = xmalloc(sizeof *v * (size_t)(n + 2));
        v[0] = "sh";
        v[1] = path;
        for (int i = 1; i <= n; i++) v[i + 1] = argv[i];
        execve("/bin/sh", v, env);
        e = errno;
    }
    sh_error("%s: %s", argv[0], strerror(e));
    child_exit(e == ENOENT ? 127 : 126);
}

/* ---- simple commands ----------------------------------------------------------------- */

static void trace(struct fields *a, char **assigns, int na)
{
    const char *ps4 = var_get("PS4");
    struct str b = { 0 };
    str_adds(&b, ps4 ? ps4 : "+ ");
    for (int i = 0; i < na; i++) { str_adds(&b, assigns[i]); str_addc(&b, ' '); }
    for (int i = 0; i < a->n; i++) { if (i) str_addc(&b, ' '); str_adds(&b, a->v[i]); }
    str_addc(&b, '\n');
    fflush(stdout);
    (void)!write(2, b.s, b.len);
    free(b.s);
}

/* name=value words, expanded; NULL on an error */
static char **expand_assigns(const struct words *w)
{
    char **out = xmalloc(sizeof *out * (size_t)(w->n + 1));
    for (int i = 0; i < w->n; i++) {
        const char *eq = strchr(w->v[i], '=');
        char *val = expand_one(eq + 1, EX_ASSIGN);
        if (!val) { for (int k = 0; k < i; k++) free(out[k]); free(out); return NULL; }
        size_t nl = (size_t)(eq - w->v[i]);
        out[i] = xmalloc(nl + strlen(val) + 2);
        memcpy(out[i], w->v[i], nl);
        out[i][nl] = '=';
        strcpy(out[i] + nl + 1, val);
        free(val);
    }
    out[w->n] = NULL;
    return out;
}

static void free_list(char **v) { if (!v) return; for (char **p = v; *p; p++) free(*p); free(v); }

/* temporary assignments (for a builtin or a function): set, and later put back */
struct tmpvar { char *name, *old; int flags, had; };

static struct tmpvar *assign_temp(char **assigns, int n)
{
    struct tmpvar *t = xmalloc(sizeof *t * (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        char *eq = strchr(assigns[i], '=');
        t[i].name = xstrndup(assigns[i], (size_t)(eq - assigns[i]));
        const char *old = var_get(t[i].name);
        t[i].had = old != NULL;
        t[i].old = old ? xstrdup(old) : NULL;
        t[i].flags = var_flags(t[i].name);
        var_set(t[i].name, eq + 1, V_EXPORT);
    }
    return t;
}

static void assign_restore(struct tmpvar *t, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (!(var_flags(t[i].name) & V_READONLY)) {
            if (t[i].had) { var_unset(t[i].name); var_set(t[i].name, t[i].old, t[i].flags); }
            else var_unset(t[i].name);
        }
        free(t[i].name);
        free(t[i].old);
    }
    free(t);
}

static int call_function(struct node *body, struct fields *a)
{
    struct params *saved = params_push(a->v + 1, a->n - 1);
    var_push_scope();
    sh.func_depth++;
    int loop_depth = sh.loop_depth;
    sh.loop_depth = 0;
    int st = exec_node(body);
    sh.loop_depth = loop_depth;
    sh.func_depth--;
    if (sh.returning) { sh.returning = 0; st = sh.status; }
    var_pop_scope();
    params_pop(saved);
    return st;
}

int exec_simple_argv(struct fields *a, int nofork);

static int exec_simple(struct node *n, int nofork)
{
    sh.lineno = n->lineno;
    int st = 0;
    unsigned runs_before = subst_runs;
    struct fields a = { 0 };
    if (expand_words(&n->args, &a) != 0) { fields_free(&a); return sh.status = 1; }
    char **assigns = expand_assigns(&n->assigns);
    if (!assigns) { fields_free(&a); return sh.status = 1; }
    if (sh.opt_x && (a.n || n->assigns.n)) trace(&a, assigns, n->assigns.n);

    if (a.n == 0) {                             /* assignments and redirections only */
        st = 0;
        for (int i = 0; i < n->assigns.n; i++) {
            char *eq = strchr(assigns[i], '=');
            *eq = 0;
            if (var_set(assigns[i], eq + 1, 0) != 0) st = 1;
            *eq = '=';
        }
        if (!st && subst_runs != runs_before) st = sh.status;       /* the last $(...)'s status */
        struct saved sv = { .n = 0 };
        if (n->redirs && apply_redirs(n->redirs, &sv) != 0) st = 1;
        restore_fds(&sv);
        reap_heredoc_writers();
        free_list(assigns);
        fields_free(&a);
        return sh.status = st;
    }

    const char *name = a.v[0];
    int special = 0;
    builtin_fn bi = builtin_find(name, &special);
    struct node *fn = special ? NULL : func_get(name);

    if (strcmp(name, "exec") == 0) {            /* redirections for good; maybe become a program */
        if (apply_redirs(n->redirs, NULL) != 0) { free_list(assigns); fields_free(&a); if (!sh.interactive) sh_exit(1); return sh.status = 1; }
        int k = 1;
        if (k < a.n && strcmp(a.v[k], "--") == 0) k++;
        if (k < a.n) {
            for (int i = 0; i < n->assigns.n; i++) { char *eq = strchr(assigns[i], '='); *eq = 0; var_set(assigns[i], eq + 1, V_EXPORT); *eq = '='; }
            fflush(NULL);
            child_setup();
            exec_program(a.v + k);
        }
        free_list(assigns);
        fields_free(&a);
        return sh.status = 0;
    }

    if (bi || fn) {
        struct saved sv = { .n = 0 };
        struct tmpvar *tmp = NULL;
        int nt = n->assigns.n;
        if (special) {
            for (int i = 0; i < nt; i++) { char *eq = strchr(assigns[i], '='); *eq = 0; var_set(assigns[i], eq + 1, 0); *eq = '='; }
        } else if (nt) {
            tmp = assign_temp(assigns, nt);
        }
        if (apply_redirs(n->redirs, &sv) != 0) {
            st = 1;
            if (special && !sh.interactive) { restore_fds(&sv); sh_exit(1); }
        } else if (fn) {
            st = call_function(fn, &a);
        } else {
            st = bi(a.n, a.v);
            fflush(stdout);
        }
        restore_fds(&sv);
        reap_heredoc_writers();
        if (tmp) assign_restore(tmp, nt);
        free_list(assigns);
        fields_free(&a);
        return sh.status = st;
    }

    /* a program */
    char path[1024];
    if (find_in_path(name, path, sizeof path) != 0) {
        sh_error("%s: not found", name);
        free_list(assigns);
        fields_free(&a);
        if (nofork) child_exit(127);
        return sh.status = 127;
    }
    fflush(NULL);
    pid_t pid = nofork ? 0 : fork();
    if (pid < 0) { sh_error("fork: %s", strerror(errno)); st = 1; }
    else if (pid == 0) {
        if (!nofork) child_setup();
        if (apply_redirs(n->redirs, NULL) != 0) child_exit(1);
        for (int i = 0; i < n->assigns.n; i++) { char *eq = strchr(assigns[i], '='); *eq = 0; var_set(assigns[i], eq + 1, V_EXPORT); *eq = '='; }
        exec_program(a.v);
    } else {
        if (sh.interactive && !sh.subshell) tcsetpgrp(0, pid);     /* ^C goes to it */
        st = wait_for(pid);
        if (sh.interactive && !sh.subshell) tcsetpgrp(0, getpid());
    }
    reap_heredoc_writers();
    free_list(assigns);
    fields_free(&a);
    return sh.status = st;
}

/* `command name args`: a builtin or a program, never a function */
int run_argv(int argc, char **argv)
{
    int special;
    builtin_fn bi = builtin_find(argv[0], &special);
    if (bi) { int st = bi(argc, argv); fflush(stdout); return st; }
    char path[1024];
    if (find_in_path(argv[0], path, sizeof path) != 0) { sh_error("%s: not found", argv[0]); return 127; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { sh_error("fork: %s", strerror(errno)); return 1; }
    if (pid == 0) { child_setup(); exec_program(argv); }
    return wait_for(pid);
}

/* ---- pipelines, lists, compound commands ---------------------------------------------- */

static int exec_inner(struct node *n, int nofork);

static int exec_pipe(struct node *n)
{
    pid_t pids[64];
    int k = n->nkids < 64 ? n->nkids : 64;
    int in_fd = -1;
    fflush(NULL);
    for (int i = 0; i < k; i++) {
        int p[2] = { -1, -1 };
        if (i + 1 < k && pipe(p) != 0) { sh_error("pipe: %s", strerror(errno)); return sh.status = 1; }
        pid_t pid = fork();
        if (pid < 0) { sh_error("fork: %s", strerror(errno)); return sh.status = 1; }
        if (pid == 0) {
            child_setup();
            if (in_fd >= 0) { dup2(in_fd, 0); close(in_fd); }
            if (p[1] >= 0) { dup2(p[1], 1); close(p[1]); close(p[0]); }
            child_exit(exec_inner(n->kids[i], 1));
        }
        pids[i] = pid;
        if (in_fd >= 0) close(in_fd);
        if (p[1] >= 0) close(p[1]);
        in_fd = p[0];
    }
    if (sh.interactive && !sh.subshell) tcsetpgrp(0, pids[k - 1]);
    int st = 0;
    for (int i = 0; i < k; i++) {
        int s = wait_for(pids[i]);
        if (i == k - 1) st = s;
    }
    if (sh.interactive && !sh.subshell) tcsetpgrp(0, getpid());
    return sh.status = st;
}

static int exec_loop(struct node *n)
{
    int st = 0;
    sh.loop_depth++;
    for (;;) {
        sh.in_condition++;
        int c = exec_node(n->cond);
        sh.in_condition--;
        if (sh.returning) break;
        if (sh.breaking) { sh.breaking--; break; }
        if (sh.continuing) { if (--sh.continuing) break; continue; }
        if ((n->type == N_WHILE) != (c == 0)) break;
        st = n->body ? exec_node(n->body) : 0;
        if (sh.returning) break;
        if (sh.breaking) { sh.breaking--; break; }
        if (sh.continuing) { if (--sh.continuing) break; continue; }
    }
    sh.loop_depth--;
    return sh.status = st;
}

static int exec_for(struct node *n)
{
    struct fields items = { 0 };
    if (n->has_in) {
        if (expand_words(&n->list, &items) != 0) { fields_free(&items); return sh.status = 1; }
    } else {
        for (int i = 0; i < params->n; i++) fields_add(&items, xstrdup(params->v[i]));
    }
    int st = 0;
    sh.loop_depth++;
    for (int i = 0; i < items.n; i++) {
        if (var_set(n->name, items.v[i], 0) != 0) { st = 1; break; }
        st = n->body ? exec_node(n->body) : 0;
        if (sh.returning) break;
        if (sh.breaking) { sh.breaking--; break; }
        if (sh.continuing) { if (--sh.continuing) break; continue; }
    }
    sh.loop_depth--;
    fields_free(&items);
    return sh.status = st;
}

static int exec_case(struct node *n)
{
    char *w = expand_one(n->word, 0);
    if (!w) return sh.status = 1;
    int st = 0;
    for (struct case_item *it = n->items; it; it = it->next) {
        int hit = 0;
        for (int i = 0; i < it->patterns.n && !hit; i++) {
            struct fields f = { 0 };
            if (expand_word(it->patterns.v[i], EX_PATTERN, &f) == 0 && f.n && pattern_match(f.v[0], w)) hit = 1;
            fields_free(&f);
        }
        if (!hit) continue;
        for (;;) {
            st = it->body ? exec_node(it->body) : 0;
            if (!it->fallthrough || !it->next) break;
            it = it->next;
        }
        break;
    }
    free(w);
    return sh.status = st;
}

static int exec_subshell(struct node *body, struct redir *redirs)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { sh_error("fork: %s", strerror(errno)); return sh.status = 1; }
    if (pid == 0) {
        child_setup();
        if (apply_redirs(redirs, NULL) != 0) child_exit(1);
        int st = exec_node(body);
        child_exit(st);
    }
    return sh.status = wait_for(pid);
}

static int set_e_check(int st)
{
    if (st && sh.opt_e && !sh.in_condition) sh_exit(st);
    return st;
}

/* nofork: we are a child that exits right after, so a program may replace it */
static int exec_inner(struct node *n, int nofork)
{
    if (!n) return sh.status = 0;
    int st = 0;
    run_traps();
    switch (n->type) {
    case N_CMD:
        st = exec_simple(n, nofork);
        return set_e_check(st);
    case N_PIPE:
        st = exec_pipe(n);
        return set_e_check(st);
    case N_NOT:
        sh.in_condition++;
        st = exec_node(n->left);
        sh.in_condition--;
        return sh.status = !st;
    case N_AND: case N_OR:
        sh.in_condition++;
        st = exec_node(n->left);
        sh.in_condition--;
        if (sh.breaking || sh.continuing || sh.returning) return st;
        if ((n->type == N_AND) == (st == 0)) st = exec_node(n->right);
        return sh.status = st;
    case N_LIST:
        exec_node(n->left);
        if (sh.breaking || sh.continuing || sh.returning) return sh.status;
        return exec_node(n->right);
    case N_BG: {
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
            child_setup();
            signal(SIGINT, SIG_IGN);            /* ^C is for the foreground */
            if (!sh.interactive) {
                int nul = open("/dev/null", O_RDONLY);
                if (nul >= 0) { dup2(nul, 0); close(nul); }
            }
            child_exit(exec_node(n->left));
        }
        if (pid < 0) { sh_error("fork: %s", strerror(errno)); return sh.status = 1; }
        sh.last_bg = pid;
        if (sh.interactive && !sh.subshell) fprintf(stderr, "[%d]\n", (int)pid);
        return sh.status = 0;
    }
    case N_SUBSHELL:
        st = exec_subshell(n->body, n->redirs);
        return set_e_check(st);
    case N_FUNC:
        func_set(n->name, n->body);
        return sh.status = 0;
    default:
        break;
    }

    /* compound commands with their redirections */
    struct saved sv = { .n = 0 };
    if (n->redirs && apply_redirs(n->redirs, &sv) != 0) { restore_fds(&sv); return sh.status = 1; }
    switch (n->type) {
    case N_GROUP: st = exec_node(n->body); break;
    case N_IF:
        sh.in_condition++;
        st = exec_node(n->cond);
        sh.in_condition--;
        if (sh.breaking || sh.continuing || sh.returning) break;
        if (st == 0) st = exec_node(n->body);
        else if (n->alt) st = exec_node(n->alt);
        else st = 0;
        break;
    case N_WHILE: case N_UNTIL: st = exec_loop(n); break;
    case N_FOR: st = exec_for(n); break;
    case N_CASE: st = exec_case(n); break;
    default: break;
    }
    restore_fds(&sv);
    return sh.status = st;
}

int exec_node(struct node *n)
{
    return exec_inner(n, 0);
}

/* ---- $(...), eval, scripts -------------------------------------------------------------- */

char *command_subst(const char *src)
{
    subst_runs++;
    int err;
    struct node *tree = parse_string(src, &err);
    if (err) { sh.status = 2; return xstrdup(""); }
    int p[2];
    if (pipe(p) != 0) { sh_error("pipe: %s", strerror(errno)); return xstrdup(""); }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        child_setup();
        close(p[0]);
        if (p[1] != 1) { dup2(p[1], 1); close(p[1]); }
        child_exit(exec_inner(tree, 1));
    }
    close(p[1]);
    struct str out = { 0 };
    char buf[4096];
    for (;;) {
        ssize_t r = read(p[0], buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        str_addn(&out, buf, (size_t)r);
    }
    close(p[0]);
    sh.status = pid > 0 ? wait_for(pid) : 1;
    while (out.len && out.s[out.len - 1] == '\n') out.s[--out.len] = 0;
    return str_take(&out);
}

int run_string(const char *s, const char *what)
{
    (void)what;
    int err;
    struct node *tree = parse_string(s, &err);
    if (err) {
        sh.status = 2;
        if (!sh.interactive) sh_exit(2);
        return 2;
    }
    if (!tree) return sh.status = 0;
    return exec_node(tree);
}

/* a script (or `.`): read it all, then command by command, so what it
 * defines early is there for what follows and a later syntax error does
 * not stop what came before */
int run_file(const char *path, int must_exist)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (must_exist) sh_error("%s: %s", path, strerror(errno));
        return -1;
    }
    struct str b = { 0 };
    char buf[8192];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) str_addn(&b, buf, r);
    fclose(f);
    struct input in;
    input_init_string(&in, b.s ? b.s : "", b.len);
    free(b.s);
    int st = 0;
    for (;;) {
        int err;
        struct node *t = parse_command(&in, &err);
        if (err) {
            st = sh.status = 2;
            if (!sh.interactive) sh_exit(2);
            break;
        }
        if (!t) break;
        if (sh.opt_n) continue;
        st = exec_node(t);
        if (sh.returning) break;
    }
    free(in.buf);
    return st;
}
