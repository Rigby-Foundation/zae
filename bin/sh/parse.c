/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Source text -> nodes, after POSIX's grammar (XCU 2.10). Words are kept as
 * written: quotes, backslashes, $..., ${...}, $(...), `...` and all, and
 * expand.c takes them apart when the command runs. To find where a $(...)
 * ends, the parser parses what is inside it (a case's `)` included) and
 * keeps the text. Here-documents are read after the newline that ends
 * their line. */
#include "sh.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum tok {
    T_EOF, T_NEWLINE, T_WORD,
    T_AND_IF, T_OR_IF,          /* && || */
    T_DSEMI, T_SEMI_AND,        /* ;; ;& */
    T_SEMI, T_AMP, T_PIPE,      /* ; & | */
    T_LPAREN, T_RPAREN,         /* ( ) */
    T_REDIR,
};

struct token {
    enum tok type;
    char *text;                 /* T_WORD: the raw word */
    enum redir_type rtype;      /* T_REDIR */
    int io_number;              /* T_REDIR: the descriptor written in front, or -1 */
    int strip;                  /* <<- */
    int lineno;
};

struct parser {
    struct input *in;
    struct token tok;           /* the lookahead */
    int have_tok;
    jmp_buf fail;
    struct redir *heredocs[64]; /* here-documents whose bodies come after the next newline */
    int nheredocs;
};

/* ---- input ------------------------------------------------------------------------ */

void input_init_string(struct input *in, const char *s, size_t len)
{
    memset(in, 0, sizeof *in);
    in->buf = xmalloc(len + 1);
    memcpy(in->buf, s, len);
    in->buf[len] = 0;
    in->len = in->cap = len;
    in->lineno = 1;
}

void input_append(struct input *in, const char *s, size_t len)
{
    if (in->len + len + 1 > in->cap) {
        in->cap = (in->len + len + 1) * 2;
        in->buf = xrealloc(in->buf, in->cap + 1);
    }
    memcpy(in->buf + in->len, s, len);
    in->len += len;
    in->buf[in->len] = 0;
}

/* The character `off` ahead, reading more input if the parser needs it; -1 at the end. */
static int pc(struct parser *p, size_t off)
{
    struct input *in = p->in;
    while (in->pos + off >= in->len) {
        if (in->eof || !in->more || in->more(in) != 0) { in->eof = 1; return -1; }
    }
    return (unsigned char)in->buf[in->pos + off];
}

static int next_char(struct parser *p)
{
    int c = pc(p, 0);
    if (c < 0) return -1;
    p->in->pos++;
    if (c == '\n') p->in->lineno++;
    return c;
}

/* ---- errors ----------------------------------------------------------------------- */

__attribute__((noreturn)) static void syntax(struct parser *p, const char *near)
{
    if (near) sh_error("line %d: syntax error near unexpected '%s'", p->in->lineno, near);
    else sh_error("line %d: syntax error: unexpected end of file", p->in->lineno);
    longjmp(p->fail, 1);
}

static const char *tok_name(const struct token *t)
{
    switch (t->type) {
    case T_EOF: return NULL;
    case T_NEWLINE: return "newline";
    case T_WORD: return t->text;
    case T_AND_IF: return "&&";
    case T_OR_IF: return "||";
    case T_DSEMI: return ";;";
    case T_SEMI_AND: return ";&";
    case T_SEMI: return ";";
    case T_AMP: return "&";
    case T_PIPE: return "|";
    case T_LPAREN: return "(";
    case T_RPAREN: return ")";
    case T_REDIR: return "redirection";
    }
    return "?";
}

/* ---- words ------------------------------------------------------------------------ */

static void scan_dollar(struct parser *p, struct str *w);
static void scan_backquote(struct parser *p, struct str *w);
static struct node *parse_list(struct parser *p, int toplevel);

/* '...': everything to the closing quote */
static void scan_squote(struct parser *p, struct str *w)
{
    str_addc(w, (char)next_char(p));
    for (;;) {
        int c = next_char(p);
        if (c < 0) syntax(p, NULL);
        str_addc(w, (char)c);
        if (c == '\'') return;
    }
}

/* "...": \ before $ ` " \ newline; $... and `...` inside */
static void scan_dquote(struct parser *p, struct str *w)
{
    str_addc(w, (char)next_char(p));
    for (;;) {
        int c = pc(p, 0);
        if (c < 0) syntax(p, NULL);
        if (c == '"') { str_addc(w, (char)next_char(p)); return; }
        if (c == '\\') {
            next_char(p);
            int d = next_char(p);
            if (d < 0) syntax(p, NULL);
            if (d == '\n') continue;                    /* a continued line */
            str_addc(w, '\\'); str_addc(w, (char)d);
            continue;
        }
        if (c == '$') { scan_dollar(p, w); continue; }
        if (c == '`') { scan_backquote(p, w); continue; }
        str_addc(w, (char)next_char(p));
    }
}

/* `...`: to the next unescaped backquote */
static void scan_backquote(struct parser *p, struct str *w)
{
    str_addc(w, (char)next_char(p));
    for (;;) {
        int c = next_char(p);
        if (c < 0) syntax(p, NULL);
        if (c == '\\') {
            int d = next_char(p);
            if (d < 0) syntax(p, NULL);
            str_addc(w, '\\'); str_addc(w, (char)d);
            continue;
        }
        str_addc(w, (char)c);
        if (c == '`') return;
    }
}

/* ${...}: to the matching brace, quotes and nesting respected */
static void scan_brace(struct parser *p, struct str *w)
{
    str_addc(w, (char)next_char(p));            /* $ */
    str_addc(w, (char)next_char(p));            /* { */
    for (;;) {
        int c = pc(p, 0);
        if (c < 0) syntax(p, NULL);
        if (c == '}') { str_addc(w, (char)next_char(p)); return; }
        if (c == '\\') { str_addc(w, (char)next_char(p)); int d = next_char(p); if (d < 0) syntax(p, NULL); str_addc(w, (char)d); continue; }
        if (c == '\'') { scan_squote(p, w); continue; }
        if (c == '"') { scan_dquote(p, w); continue; }
        if (c == '$') { scan_dollar(p, w); continue; }
        if (c == '`') { scan_backquote(p, w); continue; }
        str_addc(w, (char)next_char(p));
    }
}

/* $((...)): to the matching "))" */
static void scan_arith(struct parser *p, struct str *w)
{
    for (int i = 0; i < 3; i++) str_addc(w, (char)next_char(p));   /* $(( */
    int depth = 0;
    for (;;) {
        int c = pc(p, 0);
        if (c < 0) syntax(p, NULL);
        if (c == ')' && depth == 0 && pc(p, 1) == ')') { str_addc(w, (char)next_char(p)); str_addc(w, (char)next_char(p)); return; }
        if (c == '(') depth++;
        if (c == ')') depth--;
        if (c == '\'') { scan_squote(p, w); continue; }
        if (c == '"') { scan_dquote(p, w); continue; }
        if (c == '$') { scan_dollar(p, w); continue; }
        if (c == '`') { scan_backquote(p, w); continue; }
        str_addc(w, (char)next_char(p));
    }
}

/* $(...): parse the commands inside to find the end, keep the text */
static void scan_csubst(struct parser *p, struct str *w)
{
    size_t start = p->in->pos;
    next_char(p); next_char(p);                 /* $( */
    struct parser sub;
    memset(&sub, 0, sizeof sub);
    sub.in = p->in;
    if (setjmp(sub.fail)) longjmp(p->fail, 1);
    parse_list(&sub, 0);
    /* parse_list stopped at the ')' and left it as the lookahead */
    if (!sub.have_tok || sub.tok.type != T_RPAREN) syntax(p, sub.have_tok ? tok_name(&sub.tok) : NULL);
    if (sub.nheredocs) syntax(p, ")");           /* a here-document must end inside */
    /* the ')' token was lexed: pos is just past it */
    str_addn(w, p->in->buf + start, p->in->pos - start);
}

static void scan_dollar(struct parser *p, struct str *w)
{
    int c1 = pc(p, 1);
    if (c1 == '(') {
        if (pc(p, 2) == '(') { scan_arith(p, w); return; }
        scan_csubst(p, w);
        return;
    }
    if (c1 == '{') { scan_brace(p, w); return; }
    str_addc(w, (char)next_char(p));
}

static int is_meta(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '<' || c == '>' || c == '(' || c == ')';
}

static void read_heredocs(struct parser *p);

/* ---- tokens ----------------------------------------------------------------------- */

static void lex(struct parser *p, struct token *t)
{
    memset(t, 0, sizeof *t);
    t->io_number = -1;
    int c;
    for (;;) {                                  /* blanks, comments, continued lines */
        c = pc(p, 0);
        if (c == ' ' || c == '\t') { next_char(p); continue; }
        if (c == '\\' && pc(p, 1) == '\n') { next_char(p); next_char(p); continue; }
        if (c == '#') { while ((c = pc(p, 0)) >= 0 && c != '\n') next_char(p); continue; }
        break;
    }
    t->lineno = p->in->lineno;
    if (c < 0) { t->type = T_EOF; return; }
    if (c == '\n') {
        next_char(p);
        t->type = T_NEWLINE;
        if (p->nheredocs) read_heredocs(p);
        return;
    }
    int c1 = pc(p, 1);
    switch (c) {
    case '&': next_char(p); if (c1 == '&') { next_char(p); t->type = T_AND_IF; } else t->type = T_AMP; return;
    case '|': next_char(p); if (c1 == '|') { next_char(p); t->type = T_OR_IF; } else t->type = T_PIPE; return;
    case ';':
        next_char(p);
        if (c1 == ';') { next_char(p); t->type = T_DSEMI; }
        else if (c1 == '&') { next_char(p); t->type = T_SEMI_AND; }
        else t->type = T_SEMI;
        return;
    case '(': next_char(p); t->type = T_LPAREN; return;
    case ')': next_char(p); t->type = T_RPAREN; return;
    case '<': case '>': goto redir;
    }
    /* a word, or the descriptor number of a redirection */
    if (c >= '0' && c <= '9') {
        size_t k = 0;
        while (pc(p, k) >= '0' && pc(p, k) <= '9') k++;
        int d = pc(p, k);
        if (d == '<' || d == '>') {
            int n = 0;
            for (size_t i = 0; i < k; i++) n = n * 10 + (next_char(p) - '0');
            t->io_number = n;
            c = pc(p, 0); c1 = pc(p, 1);
            goto redir;
        }
    }
    {
        struct str w = { 0 };
        for (;;) {
            c = pc(p, 0);
            if (c < 0 || is_meta(c)) break;
            if (c == '\\') {
                if (pc(p, 1) == '\n') { next_char(p); next_char(p); continue; }
                str_addc(&w, (char)next_char(p));
                int d = next_char(p);
                if (d >= 0) str_addc(&w, (char)d);
                continue;
            }
            if (c == '\'') { scan_squote(p, &w); continue; }
            if (c == '"') { scan_dquote(p, &w); continue; }
            if (c == '$') { scan_dollar(p, &w); continue; }
            if (c == '`') { scan_backquote(p, &w); continue; }
            str_addc(&w, (char)next_char(p));
        }
        t->type = T_WORD;
        t->text = str_take(&w);
        return;
    }
redir:
    t->type = T_REDIR;
    next_char(p);
    if (c == '<') {
        if (c1 == '<') {
            next_char(p);
            t->rtype = R_HEREDOC;
            if (pc(p, 0) == '-') { next_char(p); t->strip = 1; }
        } else if (c1 == '&') { next_char(p); t->rtype = R_DUPIN; }
        else if (c1 == '>') { next_char(p); t->rtype = R_RDWR; }
        else t->rtype = R_IN;
    } else {
        if (c1 == '>') { next_char(p); t->rtype = R_APPEND; }
        else if (c1 == '&') { next_char(p); t->rtype = R_DUPOUT; }
        else if (c1 == '|') { next_char(p); t->rtype = R_CLOBBER; }
        else t->rtype = R_OUT;
    }
}

static struct token *peek(struct parser *p)
{
    if (!p->have_tok) { lex(p, &p->tok); p->have_tok = 1; }
    return &p->tok;
}

static struct token take(struct parser *p)
{
    peek(p);
    p->have_tok = 0;
    return p->tok;
}

static int peek_word(struct parser *p, const char *w)
{
    struct token *t = peek(p);
    return t->type == T_WORD && strcmp(t->text, w) == 0;
}

static void expect_word(struct parser *p, const char *w)
{
    struct token t = take(p);
    if (t.type != T_WORD || strcmp(t.text, w) != 0) syntax(p, tok_name(&t) ? tok_name(&t) : NULL);
}

static void linebreak(struct parser *p)
{
    while (peek(p)->type == T_NEWLINE) take(p);
}

/* words that end a list inside a compound command */
static int is_closer(struct parser *p)
{
    struct token *t = peek(p);
    if (t->type == T_EOF || t->type == T_RPAREN || t->type == T_DSEMI || t->type == T_SEMI_AND) return 1;
    if (t->type != T_WORD) return 0;
    static const char *const kw[] = { "then", "else", "elif", "fi", "do", "done", "esac", "}", NULL };
    for (int i = 0; kw[i]; i++) if (strcmp(t->text, kw[i]) == 0) return 1;
    return 0;
}

static int is_reserved(const char *s)
{
    static const char *const kw[] = { "if", "then", "else", "elif", "fi", "do", "done", "case", "esac", "while", "until",
                                      "for", "{", "}", "!", "in", "function", NULL };
    for (int i = 0; kw[i]; i++) if (strcmp(s, kw[i]) == 0) return 1;
    return 0;
}

/* ---- here-documents ---------------------------------------------------------------- */

/* The delimiter without its quotes; *quoted if there were any. */
static char *heredoc_delim(const char *raw, int *quoted)
{
    struct str d = { 0 };
    *quoted = 0;
    for (const char *s = raw; *s; s++) {
        if (*s == '\\' && s[1]) { *quoted = 1; str_addc(&d, *++s); continue; }
        if (*s == '\'' || *s == '"') { *quoted = 1; continue; }
        str_addc(&d, *s);
    }
    return str_take(&d);
}

static void read_heredocs(struct parser *p)
{
    for (int i = 0; i < p->nheredocs; i++) {
        struct redir *r = p->heredocs[i];
        struct str body = { 0 };
        for (;;) {
            struct str line = { 0 };
            int c;
            while ((c = next_char(p)) >= 0 && c != '\n') str_addc(&line, (char)c);
            char *l = line.s ? line.s : "";
            if (r->heredoc_strip) while (*l == '\t') l++;
            if (strcmp(l, r->heredoc_delim) == 0) { free(line.s); break; }
            str_adds(&body, l);
            str_addc(&body, '\n');
            free(line.s);
            if (c < 0) { sh_error("here-document delimited by end of file (wanted '%s')", r->heredoc_delim); break; }
        }
        r->word = body.s ? str_take(&body) : xstrdup("");
        free(r->heredoc_delim);
        r->heredoc_delim = NULL;
    }
    p->nheredocs = 0;
}

/* ---- the grammar -------------------------------------------------------------------- */

static struct node *new_node(struct parser *p, enum node_type type)
{
    struct node *n = xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->type = type;
    n->lineno = p->in->lineno;
    return n;
}

static void words_add(struct words *w, char *s)
{
    if (w->n + 1 >= w->cap) { w->cap = w->cap ? w->cap * 2 : 8; w->v = xrealloc(w->v, (size_t)w->cap * sizeof *w->v); }
    w->v[w->n++] = s;
    w->v[w->n] = NULL;
}

/* a redirection: the operator was just taken */
static struct redir *parse_redir(struct parser *p, struct token *op)
{
    struct token w = take(p);
    if (w.type != T_WORD) syntax(p, tok_name(&w));
    struct redir *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->type = op->rtype;
    r->fd = op->io_number >= 0 ? op->io_number : (op->rtype == R_IN || op->rtype == R_DUPIN || op->rtype == R_RDWR || op->rtype == R_HEREDOC) ? 0 : 1;
    if (r->type == R_HEREDOC) {
        r->heredoc_delim = heredoc_delim(w.text, &r->heredoc_quoted);
        r->heredoc_strip = op->strip;
        if (p->nheredocs == 64) syntax(p, "<<");
        p->heredocs[p->nheredocs++] = r;
    } else {
        r->word = w.text;
    }
    return r;
}

static void add_redir(struct redir **list, struct redir *r)
{
    while (*list) list = &(*list)->next;
    *list = r;
}

static void redirs_after(struct parser *p, struct node *n)
{
    while (peek(p)->type == T_REDIR) {
        struct token op = take(p);
        add_redir(&n->redirs, parse_redir(p, &op));
    }
}

static int is_assignment(const char *w)
{
    const char *eq = strchr(w, '=');
    return eq && eq > w && valid_name(w, (size_t)(eq - w));
}

static struct node *parse_command_node(struct parser *p);
static struct node *parse_compound_list(struct parser *p);

static struct node *parse_pipeline(struct parser *p)
{
    int bang = 0;
    while (peek_word(p, "!")) { take(p); bang = !bang; }
    struct node *first = parse_command_node(p);
    if (!first) syntax(p, tok_name(peek(p)));
    struct node *n = first;
    if (peek(p)->type == T_PIPE) {
        n = new_node(p, N_PIPE);
        struct node **kids = xmalloc(sizeof *kids * 2);
        int nk = 0, cap = 2;
        kids[nk++] = first;
        while (peek(p)->type == T_PIPE) {
            take(p);
            linebreak(p);
            struct node *c = parse_command_node(p);
            if (!c) syntax(p, tok_name(peek(p)));
            if (nk == cap) { cap *= 2; kids = xrealloc(kids, sizeof *kids * (size_t)cap); }
            kids[nk++] = c;
        }
        n->kids = kids;
        n->nkids = nk;
    }
    if (bang) {
        struct node *b = new_node(p, N_NOT);
        b->left = n;
        n = b;
    }
    return n;
}

static struct node *parse_and_or(struct parser *p)
{
    struct node *left = parse_pipeline(p);
    for (;;) {
        enum tok t = peek(p)->type;
        if (t != T_AND_IF && t != T_OR_IF) return left;
        take(p);
        linebreak(p);
        struct node *n = new_node(p, t == T_AND_IF ? N_AND : N_OR);
        n->left = left;
        n->right = parse_pipeline(p);
        left = n;
    }
}

/* and_or [; & newline] ... : at the top level a newline ends it (one command
 * at a time); inside a compound command it goes on to the closing word. */
static struct node *parse_list(struct parser *p, int toplevel)
{
    struct node *list = NULL;
    if (!toplevel) linebreak(p);
    for (;;) {
        if (is_closer(p) || (toplevel && peek(p)->type == T_NEWLINE)) break;
        struct node *n = parse_and_or(p);
        enum tok t = peek(p)->type;
        if (t == T_AMP) {
            take(p);
            struct node *bg = new_node(p, N_BG);
            bg->left = n;
            n = bg;
        } else if (t == T_SEMI) {
            take(p);
        }
        if (!list) list = n;
        else {
            struct node *l = new_node(p, N_LIST);
            l->left = list;
            l->right = n;
            list = l;
        }
        if (t == T_NEWLINE) {
            if (toplevel) break;
            linebreak(p);
        } else if (t != T_AMP && t != T_SEMI) {
            break;                              /* a closer, or something the caller deals with */
        } else if (!toplevel) {
            linebreak(p);
        }
    }
    return list;
}

static struct node *parse_compound_list(struct parser *p)
{
    struct node *n = parse_list(p, 0);
    if (!n) syntax(p, tok_name(peek(p)));
    return n;
}

static struct node *parse_if(struct parser *p)
{
    struct node *n = new_node(p, N_IF);
    n->cond = parse_compound_list(p);
    expect_word(p, "then");
    n->body = parse_compound_list(p);
    if (peek_word(p, "elif")) {
        take(p);
        n->alt = parse_if(p);                   /* it eats the fi */
        return n;
    }
    if (peek_word(p, "else")) {
        take(p);
        n->alt = parse_compound_list(p);
    }
    expect_word(p, "fi");
    return n;
}

static struct node *parse_do_group(struct parser *p)
{
    linebreak(p);
    expect_word(p, "do");
    struct node *b = parse_list(p, 0);          /* an empty body is accepted */
    expect_word(p, "done");
    return b;
}

static struct node *parse_for(struct parser *p)
{
    struct node *n = new_node(p, N_FOR);
    struct token name = take(p);
    if (name.type != T_WORD || !valid_name(name.text, strlen(name.text))) syntax(p, tok_name(&name));
    n->name = name.text;
    linebreak(p);
    if (peek_word(p, "in")) {
        take(p);
        n->has_in = 1;
        while (peek(p)->type == T_WORD) words_add(&n->list, take(p).text);
        if (peek(p)->type == T_SEMI) take(p);
        else if (peek(p)->type != T_NEWLINE) syntax(p, tok_name(peek(p)));
    } else if (peek(p)->type == T_SEMI) {
        take(p);
    }
    n->body = parse_do_group(p);
    return n;
}

static struct node *parse_case(struct parser *p)
{
    struct node *n = new_node(p, N_CASE);
    struct token w = take(p);
    if (w.type != T_WORD) syntax(p, tok_name(&w));
    n->word = w.text;
    linebreak(p);
    expect_word(p, "in");
    linebreak(p);
    struct case_item **tail = &n->items;
    while (!peek_word(p, "esac")) {
        struct case_item *it = xmalloc(sizeof *it);
        memset(it, 0, sizeof *it);
        if (peek(p)->type == T_LPAREN) take(p);
        for (;;) {
            struct token pat = take(p);
            if (pat.type != T_WORD) syntax(p, tok_name(&pat));
            words_add(&it->patterns, pat.text);
            if (peek(p)->type == T_PIPE) { take(p); continue; }
            break;
        }
        struct token rp = take(p);
        if (rp.type != T_RPAREN) syntax(p, tok_name(&rp));
        it->body = parse_list(p, 0);
        enum tok t = peek(p)->type;
        if (t == T_DSEMI || t == T_SEMI_AND) {
            take(p);
            it->fallthrough = t == T_SEMI_AND;
        } else if (!peek_word(p, "esac")) {
            syntax(p, tok_name(peek(p)));
        }
        linebreak(p);
        *tail = it;
        tail = &it->next;
    }
    take(p);                                    /* esac */
    return n;
}

/* a compound command (its reserved word or bracket is the lookahead), or NULL */
static struct node *parse_compound(struct parser *p)
{
    struct token *t = peek(p);
    struct node *n = NULL;
    if (t->type == T_LPAREN) {
        take(p);
        n = new_node(p, N_SUBSHELL);
        n->body = parse_compound_list(p);
        struct token rp = take(p);
        if (rp.type != T_RPAREN) syntax(p, tok_name(&rp));
    } else if (t->type == T_WORD) {
        if (strcmp(t->text, "{") == 0) {
            take(p);
            n = new_node(p, N_GROUP);
            n->body = parse_compound_list(p);
            expect_word(p, "}");
        } else if (strcmp(t->text, "if") == 0) {
            take(p);
            n = parse_if(p);
        } else if (strcmp(t->text, "while") == 0 || strcmp(t->text, "until") == 0) {
            int until = t->text[0] == 'u';
            take(p);
            n = new_node(p, until ? N_UNTIL : N_WHILE);
            n->cond = parse_compound_list(p);
            n->body = parse_do_group(p);
        } else if (strcmp(t->text, "for") == 0) {
            take(p);
            n = parse_for(p);
        } else if (strcmp(t->text, "case") == 0) {
            take(p);
            n = parse_case(p);
        }
    }
    if (n) redirs_after(p, n);
    return n;
}

/* a simple command, a compound one, or a function definition; NULL if
 * the lookahead starts none of them */
static struct node *parse_command_node(struct parser *p)
{
    struct token *t = peek(p);
    struct node *n = parse_compound(p);
    if (n) return n;

    t = peek(p);
    if (t->type == T_WORD && strcmp(t->text, "function") == 0) {   /* function name [()] body: bash's form */
        take(p);
        struct token name = take(p);
        if (name.type != T_WORD) syntax(p, tok_name(&name));
        if (peek(p)->type == T_LPAREN) { take(p); struct token rp = take(p); if (rp.type != T_RPAREN) syntax(p, tok_name(&rp)); }
        linebreak(p);
        n = new_node(p, N_FUNC);
        n->name = name.text;
        n->body = parse_compound(p);
        if (!n->body) syntax(p, tok_name(peek(p)));
        return n;
    }

    n = new_node(p, N_CMD);
    int any = 0;
    for (;;) {
        t = peek(p);
        if (t->type == T_REDIR) {
            struct token op = take(p);
            add_redir(&n->redirs, parse_redir(p, &op));
            any = 1;
            continue;
        }
        if (t->type != T_WORD) break;
        if (n->args.n == 0 && is_assignment(t->text)) {
            words_add(&n->assigns, take(p).text);
            any = 1;
            continue;
        }
        if (n->args.n == 0 && n->assigns.n == 0 && !n->redirs && is_reserved(t->text)) {
            if (!any) return NULL;              /* a reserved word where a command should start: the caller's */
            break;
        }
        struct token w = take(p);
        if (n->args.n == 0 && n->assigns.n == 0 && !n->redirs && peek(p)->type == T_LPAREN) {
            /* name() compound-command: a function */
            take(p);
            struct token rp = take(p);
            if (rp.type != T_RPAREN) syntax(p, tok_name(&rp));
            if (!valid_name(w.text, strlen(w.text))) {
                /* sh allows only names; accept what bash does (a-b, a.b), not quoted ones */
                for (const char *c = w.text; *c; c++) if (strchr("'\"$`\\", *c)) syntax(p, w.text);
            }
            linebreak(p);
            struct node *f = new_node(p, N_FUNC);
            f->name = w.text;
            f->body = parse_compound(p);
            if (!f->body) syntax(p, tok_name(peek(p)));
            return f;
        }
        words_add(&n->args, w.text);
        any = 1;
    }
    if (!any) return NULL;
    return n;
}

/* ---- entry points ------------------------------------------------------------------- */

struct node *parse_command(struct input *in, int *err)
{
    struct parser p;
    memset(&p, 0, sizeof p);
    p.in = in;
    *err = 0;
    if (setjmp(p.fail)) {
        *err = 1;
        /* forget the rest of the line it happened on */
        while (in->pos < in->len && in->buf[in->pos] != '\n') in->pos++;
        if (in->pos < in->len) { in->pos++; in->lineno++; }
        return NULL;
    }
    for (;;) {
        struct token *t = peek(&p);
        if (t->type == T_EOF) return NULL;
        if (t->type == T_NEWLINE) { take(&p); continue; }
        break;
    }
    struct node *n = parse_list(&p, 1);
    struct token *t = peek(&p);
    if (t->type == T_NEWLINE) take(&p);
    else if (t->type != T_EOF) syntax(&p, tok_name(t));
    if (!n) syntax(&p, tok_name(peek(&p)));
    return n;
}

struct node *parse_string(const char *s, int *err)
{
    struct input in;
    input_init_string(&in, s, strlen(s));
    struct parser p;
    memset(&p, 0, sizeof p);
    p.in = &in;
    *err = 0;
    if (setjmp(p.fail)) { *err = 1; return NULL; }
    struct node *list = NULL;
    for (;;) {
        linebreak(&p);
        if (peek(&p)->type == T_EOF) break;
        struct node *n = parse_list(&p, 1);
        struct token *t = peek(&p);
        if (t->type == T_NEWLINE) take(&p);
        else if (t->type != T_EOF) syntax(&p, tok_name(t));
        if (!n) continue;
        if (!list) list = n;
        else {
            struct node *l = new_node(&p, N_LIST);
            l->left = list;
            l->right = n;
            list = l;
        }
    }
    return list;
}

/* Where the $(...) starting at raw[pos] ends: the index just past its ')'.
 * The word was parsed once already, so it is well formed. */
size_t csubst_end(const char *raw, size_t pos)
{
    struct input in;
    input_init_string(&in, raw + pos, strlen(raw + pos));
    struct parser p;
    memset(&p, 0, sizeof p);
    p.in = &in;
    struct str w = { 0 };
    size_t end = strlen(raw);
    if (!setjmp(p.fail)) { scan_csubst(&p, &w); end = pos + in.pos; }
    free(w.s);
    free(in.buf);
    return end;
}

/* ---- sh --dump ---------------------------------------------------------------------- */

static void ind(int d) { for (int i = 0; i < d; i++) fputs("  ", stdout); }

static void dump_redirs(const struct redir *r, int depth)
{
    static const char *const ops[] = { "<", ">", ">>", ">|", "<>", "<&", ">&", "<<" };
    for (; r; r = r->next) {
        ind(depth);
        if (r->type == R_HEREDOC) printf("redir %d<<%s%s: [%s]\n", r->fd, r->heredoc_strip ? "-" : "", r->heredoc_quoted ? " (quoted)" : "", r->word);
        else printf("redir %d%s %s\n", r->fd, ops[r->type], r->word);
    }
}

void dump_node(const struct node *n, int depth)
{
    if (!n) { ind(depth); puts("(empty)"); return; }
    ind(depth);
    switch (n->type) {
    case N_CMD:
        printf("cmd");
        for (int i = 0; i < n->assigns.n; i++) printf(" {%s}", n->assigns.v[i]);
        for (int i = 0; i < n->args.n; i++) printf(" [%s]", n->args.v[i]);
        putchar('\n');
        break;
    case N_PIPE: puts("pipe"); for (int i = 0; i < n->nkids; i++) dump_node(n->kids[i], depth + 1); break;
    case N_NOT: puts("not"); dump_node(n->left, depth + 1); break;
    case N_AND: case N_OR: case N_LIST:
        puts(n->type == N_AND ? "and" : n->type == N_OR ? "or" : "list");
        dump_node(n->left, depth + 1); dump_node(n->right, depth + 1);
        break;
    case N_BG: puts("background"); dump_node(n->left, depth + 1); break;
    case N_SUBSHELL: puts("subshell"); dump_node(n->body, depth + 1); break;
    case N_GROUP: puts("group"); dump_node(n->body, depth + 1); break;
    case N_IF:
        puts("if"); dump_node(n->cond, depth + 1);
        ind(depth); puts("then"); dump_node(n->body, depth + 1);
        if (n->alt) { ind(depth); puts("else"); dump_node(n->alt, depth + 1); }
        break;
    case N_WHILE: case N_UNTIL:
        puts(n->type == N_WHILE ? "while" : "until"); dump_node(n->cond, depth + 1);
        ind(depth); puts("do"); dump_node(n->body, depth + 1);
        break;
    case N_FOR:
        printf("for %s", n->name);
        if (n->has_in) { printf(" in"); for (int i = 0; i < n->list.n; i++) printf(" [%s]", n->list.v[i]); }
        putchar('\n');
        dump_node(n->body, depth + 1);
        break;
    case N_CASE:
        printf("case [%s]\n", n->word);
        for (const struct case_item *it = n->items; it; it = it->next) {
            ind(depth + 1);
            for (int i = 0; i < it->patterns.n; i++) printf("%s[%s]", i ? "|" : "", it->patterns.v[i]);
            puts(it->fallthrough ? " ;&" : "");
            dump_node(it->body, depth + 2);
        }
        break;
    case N_FUNC: printf("function %s\n", n->name); dump_node(n->body, depth + 1); break;
    }
    dump_redirs(n->redirs, depth + 1);
}
