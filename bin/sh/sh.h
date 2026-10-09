/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* sh: the sic shell. A POSIX shell (the Shell Command Language of POSIX.1-2017,
 * enough for configure scripts and Makefiles), plus what ZAE's first shell
 * did for people at the console: a prompt with the directory, ^C for the
 * foreground job, "[exit N]" after a failing command, `help`.
 *
 *   parse.c    source text -> a tree of nodes (words stay as written)
 *   expand.c   a word -> fields: parameters, $(...), $((...)), splitting, globs
 *   arith.c    $((...)) arithmetic
 *   exec.c     running the tree: pipelines, redirections, functions, traps
 *   vars.c     variables, positional parameters, the environment
 *   builtins.c cd, set, read, eval, test, printf, ... (the ones that must be,
 *              and the ones that save a fork)
 *   main.c     arguments, the prompt, reading commands */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ---- the tree ------------------------------------------------------------------ */

enum redir_type {
    R_IN,           /* <   */
    R_OUT,          /* >   */
    R_APPEND,       /* >>  */
    R_CLOBBER,      /* >|  */
    R_RDWR,         /* <>  */
    R_DUPIN,        /* <&  */
    R_DUPOUT,       /* >&  */
    R_HEREDOC,      /* << and <<- (the body already read) */
};

struct redir {
    struct redir *next;
    int fd;                     /* the descriptor redirected (the default filled in) */
    enum redir_type type;
    char *word;                 /* the file, the descriptor, or (here-doc) the body */
    int heredoc_quoted;         /* the delimiter was quoted: the body is not expanded */
    int heredoc_strip;          /* <<-: leading tabs go (done while reading) */
    char *heredoc_delim;        /* while the parser still waits for the body */
};

struct words {                  /* raw words, as written (quotes, $..., `...` and all) */
    char **v;
    int n, cap;
};

enum node_type {
    N_CMD,                      /* assignments, words, redirections */
    N_PIPE,                     /* kids: the commands; bang: ! in front */
    N_AND, N_OR,                /* left && right, left || right */
    N_LIST,                     /* left ; right (left may be N_BG) */
    N_BG,                       /* left & */
    N_SUBSHELL,                 /* ( body ) */
    N_GROUP,                    /* { body } */
    N_IF,                       /* if cond then body [else alt] fi (elif: alt is another N_IF) */
    N_WHILE, N_UNTIL,           /* cond, body */
    N_FOR,                      /* name, words (NULL: "$@"), body */
    N_CASE,                     /* word; items */
    N_FUNC,                     /* name() body */
    N_NOT,                      /* ! pipeline */
};

struct case_item {
    struct case_item *next;
    struct words patterns;
    struct node *body;          /* may be NULL */
    int fallthrough;            /* ;& (not POSIX, accepted) */
};

struct node {
    enum node_type type;
    int lineno;
    struct redir *redirs;       /* compound commands may have them too */
    /* N_CMD */
    struct words assigns, args;
    /* N_PIPE */
    struct node **kids;
    int nkids;
    /* binary nodes, bodies */
    struct node *left, *right;  /* N_AND/OR/LIST: both; N_BG/N_NOT: left */
    struct node *cond, *body, *alt;
    /* N_FOR, N_FUNC */
    char *name;
    struct words list;          /* N_FOR: the words after `in` */
    int has_in;                 /* N_FOR: `in` was there (else "$@") */
    /* N_CASE */
    char *word;
    struct case_item *items;
};

/* ---- reading and parsing ---------------------------------------------------------- */

/* The text being parsed. `more` (if any) appends another line when the
 * parser needs one to finish a command (a terminal: prompt with PS2). */
struct input {
    char *buf;
    size_t len, cap, pos;
    int lineno;
    int (*more)(struct input *in);          /* 0: got a line, -1: end of input */
    int eof;
};

void input_init_string(struct input *in, const char *s, size_t len);
void input_append(struct input *in, const char *s, size_t len);

/* The next complete command (up to a newline at the top level), or NULL at
 * the end of the input. *err is set on a syntax error (a message printed). */
struct node *parse_command(struct input *in, int *err);
/* All of a string (eval, $(...), sh -c): a list, or NULL if it is empty. */
struct node *parse_string(const char *s, int *err);
void dump_node(const struct node *n, int depth);   /* sh --dump: the tree, for debugging */
size_t csubst_end(const char *raw, size_t pos);   /* raw[pos] is the $ of a $(...): the index past its ) */

/* ---- variables -------------------------------------------------------------------- */

#define V_EXPORT    1
#define V_READONLY  2

void        vars_init(char **envp);
const char *var_get(const char *name);
int         var_set(const char *name, const char *value, int flags);   /* -1: readonly */
int         var_unset(const char *name);
void        var_export(const char *name);
int         var_flags(const char *name);
void        var_readonly(const char *name);
char      **var_environ(void);              /* for exec: NAME=value of the exported ones (malloc'd) */
void        var_print(int exported_only, int readonly_only);
int         valid_name(const char *s, size_t len);
/* function-local variables: a scope per call, `local` adds to the innermost */
void        var_push_scope(void);
void        var_pop_scope(void);
int         var_local(const char *name);

/* Positional parameters: $0 stays, $1... per function call or `set --`. */
struct params { char **v; int n; };
extern struct params *params;
extern char *arg0;
void        params_set(char **v, int n);    /* copies */
struct params *params_push(char **v, int n);
void        params_pop(struct params *saved);

/* functions */
struct node *func_get(const char *name);
void        func_set(const char *name, struct node *body);
int         func_unset(const char *name);

/* ---- expansion -------------------------------------------------------------------- */

struct fields { char **v; int n, cap; };
void   fields_add(struct fields *f, char *s);   /* takes s */
void   fields_free(struct fields *f);

#define EX_SPLIT    1           /* field splitting and pathname expansion (not for assignments, case words, redirections) */
#define EX_ASSIGN   2           /* tilde after = and : too */
#define EX_PATTERN  4           /* keep quoting as backslashes: the result is a pattern (case, ${x%pat}) */
#define EX_HEREDOC  8           /* a here-document body: only $, ` and \ are special */

/* raw word -> fields (one, unless EX_SPLIT); 0, or -1 on an error (message printed) */
int    expand_word(const char *raw, int flags, struct fields *out);
char  *expand_one(const char *raw, int flags);  /* one string (fields joined by spaces), NULL on an error */
int    expand_words(const struct words *w, struct fields *out);   /* the arguments of a command */
char  *command_subst(const char *src);          /* run, capture, strip trailing newlines */
int    pattern_match(const char *pat, const char *s);   /* sh patterns; \x is x literally */
long   arith_eval(const char *expr, int *err);

/* ---- running ---------------------------------------------------------------------- */

struct shell {
    int status;                 /* $? */
    pid_t pid;                  /* $$: the shell's, also in its subshells */
    pid_t last_bg;              /* $! */
    int interactive;
    int opt_e, opt_u, opt_x, opt_f, opt_n, opt_v, opt_C, opt_a;
    int breaking, continuing;   /* loops to leave / restart */
    int returning;              /* a function (or a sourced file) is returning */
    int loop_depth, func_depth, source_depth;
    int in_condition;           /* set -e does not apply (if/while conditions, && || left sides, !) */
    int subshell;               /* we are a forked copy: exit, don't return to a prompt */
    int lineno;
    char *traps[65];            /* 0: EXIT; NULL: default; "": ignored */
    volatile int pending_sig[65];
    volatile int any_pending;
};
extern struct shell sh;

int  exec_node(struct node *n);
int  run_string(const char *s, const char *what);   /* eval, sh -c */
int  run_file(const char *path, int must_exist);    /* ., a script */
void run_traps(void);
void sh_exit(int status) __attribute__((noreturn));  /* the EXIT trap, then exit */
int  find_in_path(const char *name, char *out, size_t outlen);
int  run_argv(int argc, char **argv);            /* command: a builtin or a program, not a function */
void install_trap(int sig, const char *action);
int  signal_number(const char *name);
const char *signal_name(int sig);

/* builtins: argv[0] is the name; return the status */
typedef int (*builtin_fn)(int argc, char **argv);
builtin_fn builtin_find(const char *name, int *special);
int  builtin_test(int argc, char **argv);
int  builtin_printf(int argc, char **argv);
int  sh_set_args(int argc, char **argv);          /* set's options and operands (also sh's own command line) */

/* errors: "sh: ..." on stderr */
void sh_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* a growable string */
struct str { char *s; size_t len, cap; };
void str_addc(struct str *b, char c);
void str_addn(struct str *b, const char *s, size_t n);
void str_adds(struct str *b, const char *s);
char *str_take(struct str *b);              /* the string (NUL-terminated); b empty again */
