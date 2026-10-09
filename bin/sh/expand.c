/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Word expansion (XCU 2.6): tilde, parameters, command substitution,
 * arithmetic, then field splitting, then pathname expansion, with the
 * quotes taken out on the way. Every character produced carries where it
 * came from:
 *   L  written in the word, unquoted: globbed, never split
 *   E  produced by an unquoted expansion: split on IFS, globbed
 *   Q  quoted (or produced inside quotes): neither
 * "$@" breaks fields by itself. */
#include "sh.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void fields_add(struct fields *f, char *s)
{
    if (f->n + 1 >= f->cap) { f->cap = f->cap ? f->cap * 2 : 8; f->v = xrealloc(f->v, sizeof *f->v * (size_t)f->cap); }
    f->v[f->n++] = s;
    f->v[f->n] = NULL;
}

void fields_free(struct fields *f)
{
    for (int i = 0; i < f->n; i++) free(f->v[i]);
    free(f->v);
    memset(f, 0, sizeof *f);
}

/* ---- the field being built ---------------------------------------------------------- */

struct pre {                    /* a field before splitting: text and, per character, its tag */
    struct str s, m;
    int exists;                 /* quotes were written: an empty field stays */
};

struct ex {
    int flags;
    struct pre *pres;           /* finished fields ("$@" breaks) */
    int npres, cap;
    struct pre cur;
    int error;
    char lit;                   /* the tag of unquoted literal text: L, or E inside an unquoted ${x:-word} */
};

static void put(struct ex *x, char c, char tag)
{
    str_addc(&x->cur.s, c);
    str_addc(&x->cur.m, tag);
}

static void puts_tag(struct ex *x, const char *s, char tag)
{
    for (; *s; s++) put(x, *s, tag);
}

static void field_break(struct ex *x)
{
    if (x->npres == x->cap) { x->cap = x->cap ? x->cap * 2 : 4; x->pres = xrealloc(x->pres, sizeof *x->pres * (size_t)x->cap); }
    x->pres[x->npres++] = x->cur;
    memset(&x->cur, 0, sizeof x->cur);
}

/* ---- finding the ends of things in a word ------------------------------------------- */

/* raw[i] opens a '...' quote: the index past its end */
static size_t skip_squote(const char *raw, size_t i)
{
    i++;
    while (raw[i] && raw[i] != '\'') i++;
    return raw[i] ? i + 1 : i;
}

static size_t skip_dollar(const char *raw, size_t i);

static size_t skip_backquote(const char *raw, size_t i)
{
    i++;
    while (raw[i] && raw[i] != '`') { if (raw[i] == '\\' && raw[i + 1]) i++; i++; }
    return raw[i] ? i + 1 : i;
}

static size_t skip_dquote(const char *raw, size_t i)
{
    i++;
    while (raw[i] && raw[i] != '"') {
        if (raw[i] == '\\' && raw[i + 1]) { i += 2; continue; }
        if (raw[i] == '$') { i = skip_dollar(raw, i); continue; }
        if (raw[i] == '`') { i = skip_backquote(raw, i); continue; }
        i++;
    }
    return raw[i] ? i + 1 : i;
}

/* raw[i] is '$': the index past what it starts */
static size_t skip_dollar(const char *raw, size_t i)
{
    if (raw[i + 1] == '(') {
        if (raw[i + 2] == '(') {                /* $(( ... )) */
            size_t j = i + 3;
            int depth = 0;
            while (raw[j]) {
                if (raw[j] == ')' && depth == 0 && raw[j + 1] == ')') return j + 2;
                if (raw[j] == '(') depth++;
                else if (raw[j] == ')') depth--;
                else if (raw[j] == '\'') { j = skip_squote(raw, j); continue; }
                else if (raw[j] == '"') { j = skip_dquote(raw, j); continue; }
                else if (raw[j] == '$') { j = skip_dollar(raw, j); continue; }
                else if (raw[j] == '`') { j = skip_backquote(raw, j); continue; }
                j++;
            }
            return j;
        }
        return csubst_end(raw, i);
    }
    if (raw[i + 1] == '{') {
        size_t j = i + 2;
        while (raw[j] && raw[j] != '}') {
            if (raw[j] == '\\' && raw[j + 1]) { j += 2; continue; }
            if (raw[j] == '\'') { j = skip_squote(raw, j); continue; }
            if (raw[j] == '"') { j = skip_dquote(raw, j); continue; }
            if (raw[j] == '$') { j = skip_dollar(raw, j); continue; }
            if (raw[j] == '`') { j = skip_backquote(raw, j); continue; }
            j++;
        }
        return raw[j] ? j + 1 : j;
    }
    return i + 1;
}

/* ---- parameters --------------------------------------------------------------------- */

static int is_special(char c) { return c == '@' || c == '*' || c == '#' || c == '?' || c == '-' || c == '$' || c == '!' || c == '0'; }

static char *ifs_join(void)
{
    const char *ifs = var_get("IFS");
    char sep = !ifs ? ' ' : ifs[0];
    struct str b = { 0 };
    for (int i = 0; i < params->n; i++) {
        if (i && sep) str_addc(&b, sep);
        str_adds(&b, params->v[i]);
    }
    return str_take(&b);
}

static char *opts_string(void)
{
    struct str b = { 0 };
    if (sh.opt_a) str_addc(&b, 'a');
    if (sh.opt_C) str_addc(&b, 'C');
    if (sh.opt_e) str_addc(&b, 'e');
    if (sh.opt_f) str_addc(&b, 'f');
    if (sh.interactive) str_addc(&b, 'i');
    if (sh.opt_n) str_addc(&b, 'n');
    if (sh.opt_u) str_addc(&b, 'u');
    if (sh.opt_v) str_addc(&b, 'v');
    if (sh.opt_x) str_addc(&b, 'x');
    return str_take(&b);
}

/* The value of a parameter (name is a name, digits, or one special
 * character), malloc'd; NULL if it is unset. */
static char *param_value(const char *name)
{
    char num[32];
    if (name[1] == 0 && is_special(name[0])) {
        switch (name[0]) {
        case '@': case '*': return params->n ? ifs_join() : NULL;
        case '#': snprintf(num, sizeof num, "%d", params->n); return xstrdup(num);
        case '?': snprintf(num, sizeof num, "%d", sh.status); return xstrdup(num);
        case '-': return opts_string();
        case '$': snprintf(num, sizeof num, "%d", (int)sh.pid); return xstrdup(num);
        case '!': if (!sh.last_bg) return NULL; snprintf(num, sizeof num, "%d", (int)sh.last_bg); return xstrdup(num);
        case '0': return xstrdup(arg0 ? arg0 : "sh");
        }
    }
    if (name[0] >= '0' && name[0] <= '9') {
        int k = atoi(name);
        if (k == 0) return xstrdup(arg0 ? arg0 : "sh");
        return k <= params->n ? xstrdup(params->v[k - 1]) : NULL;
    }
    if (strcmp(name, "LINENO") == 0) { snprintf(num, sizeof num, "%d", sh.lineno); return xstrdup(num); }
    const char *v = var_get(name);
    return v ? xstrdup(v) : NULL;
}

static void expand_into(struct ex *x, const char *raw, size_t len, int dq);

/* A string given back by an expansion: tagged E outside quotes, Q inside. */
static void put_value(struct ex *x, const char *v, int dq)
{
    puts_tag(x, v, dq ? 'Q' : 'E');
}

/* "$@" (dq) and unquoted $@ / $*: a field per positional parameter */
static void put_params(struct ex *x, int dq, char which)
{
    if (dq && which == '*') {
        char *j = ifs_join();
        puts_tag(x, j, 'Q');
        free(j);
        return;
    }
    for (int i = 0; i < params->n; i++) {
        if (i) field_break(x);
        if (dq) x->cur.exists = 1;
        puts_tag(x, params->v[i], dq ? 'Q' : 'E');
    }
}

/* the word of ${name op word} as one string (no splitting); with
 * EX_PATTERN, quoted pattern characters come back escaped */
static char *sub_word(const char *w, size_t len, int dq, int flags)
{
    struct ex y;
    memset(&y, 0, sizeof y);
    y.flags = flags & ~(EX_SPLIT | EX_HEREDOC);
    y.lit = 'L';
    expand_into(&y, w, len, dq);
    struct str out = { 0 };
    for (int i = 0; i <= y.npres; i++) {
        struct pre *pr = i < y.npres ? &y.pres[i] : &y.cur;
        if (i) str_addc(&out, ' ');
        for (size_t k = 0; k < pr->s.len; k++) {
            if ((flags & EX_PATTERN) && pr->m.s[k] == 'Q' && strchr("*?[]\\", pr->s.s[k])) str_addc(&out, '\\');
            str_addc(&out, pr->s.s[k]);
        }
        free(pr->s.s); free(pr->m.s);
    }
    free(y.pres);
    if (y.error) { free(out.s); return NULL; }
    return str_take(&out);
}

/* the word of ${x-word} / ${x+word} into the current field: unquoted, its
 * literal text is subject to splitting like any expansion result */
static void word_into(struct ex *x, const char *w, size_t len, int dq)
{
    char saved = x->lit;
    if (!dq) x->lit = 'E';
    expand_into(x, w, len, dq);
    x->lit = saved;
}

/* The longest or shortest prefix / suffix of v matching pat, removed. */
static char *remove_affix(const char *v, const char *pat, int suffix, int longest)
{
    size_t n = strlen(v);
    char *tmp = xmalloc(n + 1);
    if (!suffix) {
        for (size_t k = 0; k <= n; k++) {
            size_t len = longest ? n - k : k;
            memcpy(tmp, v, len); tmp[len] = 0;
            if (pattern_match(pat, tmp)) { free(tmp); return xstrdup(v + len); }
        }
    } else {
        for (size_t k = 0; k <= n; k++) {
            size_t start = longest ? k : n - k;
            if (pattern_match(pat, v + start)) { free(tmp); return xstrndup(v, start); }
        }
    }
    free(tmp);
    return xstrdup(v);
}

/* ${...}: inner is what is between the braces */
static void expand_brace(struct ex *x, const char *inner, size_t len, int dq)
{
    char *in = xstrndup(inner, len);
    char name[256];
    size_t nl = 0, i = 0;
    int length = 0;
    /* a parameter's name at in+k: a name, digits, or one special character */
    #define NAME_AT(k) do { \
        size_t _k = (k); nl = 0; \
        if (in[_k] >= '0' && in[_k] <= '9') { while (in[_k] >= '0' && in[_k] <= '9' && nl < sizeof name - 1) name[nl++] = in[_k++]; } \
        else if (in[_k] && is_special(in[_k])) name[nl++] = in[_k++]; \
        else while ((in[_k] == '_' || (in[_k] >= 'a' && in[_k] <= 'z') || (in[_k] >= 'A' && in[_k] <= 'Z') || (nl && in[_k] >= '0' && in[_k] <= '9')) && nl < sizeof name - 1) name[nl++] = in[_k++]; \
        name[nl] = 0; i = _k; } while (0)
    if (in[0] == '#' && in[1]) {                /* ${#name}: its length, if a bare name follows */
        NAME_AT(1);
        if (nl && in[i] == 0) length = 1;
    }
    if (!length) NAME_AT(0);
    #undef NAME_AT
    if (!nl) { sh_error("${%s}: bad substitution", in); x->error = 1; free(in); return; }

    char *val = param_value(name);
    int is_at = nl == 1 && (name[0] == '@' || name[0] == '*');
    if (length) {
        char num[32];
        size_t l = val ? strlen(val) : 0;
        if (is_at) l = (size_t)params->n;
        snprintf(num, sizeof num, "%zu", l);
        put_value(x, num, dq);
        free(val); free(in);
        return;
    }
    const char *op = in + i;
    const char *word = op;
    int colon = 0;
    char kind = 0;
    if (*op == ':' && op[1] && strchr("-=?+", op[1])) { colon = 1; kind = op[1]; word = op + 2; }
    else if (*op && strchr("-=?+", *op)) { kind = *op; word = op + 1; }
    else if (*op == '%' || *op == '#') { kind = *op; word = op + 1; if (*word == *op) { kind = *op == '%' ? 'P' : 'H'; word++; } }
    else if (*op) { sh_error("${%s}: bad substitution", in); x->error = 1; free(val); free(in); return; }
    size_t wl = strlen(word);
    int set = val != NULL && !(colon && val[0] == 0);
    if (is_at && params->n == 0) set = 0;

    switch (kind) {
    case 0:
        if (!val && sh.opt_u && !is_at) { sh_error("%s: parameter not set", name); x->error = 1; break; }
        if (is_at) put_params(x, dq, name[0]);
        else if (val) put_value(x, val, dq);
        break;
    case '-':
        if (set) { if (is_at) put_params(x, dq, name[0]); else put_value(x, val, dq); }
        else word_into(x, word, wl, dq);
        break;
    case '+':
        if (set) word_into(x, word, wl, dq);
        break;
    case '=':
        if (set) { put_value(x, val, dq); break; }
        {
            char *w = sub_word(word, wl, dq, 0);
            if (!w) { x->error = 1; break; }
            if (!valid_name(name, nl)) { sh_error("%s: cannot assign in this way", name); x->error = 1; free(w); break; }
            var_set(name, w, 0);
            put_value(x, w, dq);
            free(w);
        }
        break;
    case '?':
        if (set) { put_value(x, val, dq); break; }
        {
            char *w = sub_word(word, wl, dq, 0);
            sh_error("%s: %s", name, w && *w ? w : "parameter null or not set");
            free(w);
            x->error = 2;                       /* fatal in a script */
        }
        break;
    case '%': case 'P': case '#': case 'H': {
        if (!val) { if (sh.opt_u) { sh_error("%s: parameter not set", name); x->error = 1; } break; }
        /* the pattern's own quotes make characters literal; double quotes around the whole ${...} don't */
        char *pat = sub_word(word, wl, 0, EX_PATTERN);
        if (!pat) { x->error = 1; break; }
        char *r = remove_affix(val, pat, kind == '%' || kind == 'P', kind == 'P' || kind == 'H');
        put_value(x, r, dq);
        free(r); free(pat);
        break;
    }
    }
    free(val);
    free(in);
}

/* ---- the walk over a word ------------------------------------------------------------- */

static void do_csubst(struct ex *x, const char *src, int dq)
{
    char *out = command_subst(src);
    put_value(x, out, dq);
    free(out);
}

/* `...`: backslash keeps its meaning only before $ ` \ (and " inside "...") */
static void do_backquote(struct ex *x, const char *raw, size_t a, size_t b, int dq)
{
    struct str src = { 0 };
    for (size_t i = a; i < b; i++) {
        if (raw[i] == '\\' && i + 1 < b && (raw[i + 1] == '$' || raw[i + 1] == '`' || raw[i + 1] == '\\' || (dq && raw[i + 1] == '"'))) {
            str_addc(&src, raw[++i]);
            continue;
        }
        str_addc(&src, raw[i]);
    }
    char *s = str_take(&src);
    do_csubst(x, s, dq);
    free(s);
}

static void do_arith(struct ex *x, const char *expr, size_t len, int dq)
{
    char *e = sub_word(expr, len, 1, 0);
    if (!e) { x->error = 1; return; }
    int err = 0;
    long v = arith_eval(e, &err);
    free(e);
    if (err) { x->error = 2; return; }
    char num[32];
    snprintf(num, sizeof num, "%ld", v);
    put_value(x, num, dq);
}

static void do_dollar(struct ex *x, const char *raw, size_t *ip, int dq)
{
    size_t i = *ip;
    char c = raw[i + 1];
    if (c == '(') {
        size_t end = skip_dollar(raw, i);
        if (raw[i + 2] == '(') do_arith(x, raw + i + 3, end - i - 5, dq);
        else {
            char *src = xstrndup(raw + i + 2, end - i - 3);
            do_csubst(x, src, dq);
            free(src);
        }
        *ip = end;
        return;
    }
    if (c == '{') {
        size_t end = skip_dollar(raw, i);
        expand_brace(x, raw + i + 2, end - i - 3, dq);
        *ip = end;
        return;
    }
    char name[256];
    size_t nl = 0, j = i + 1;
    if (c && (is_special(c) || (c >= '1' && c <= '9'))) {
        name[nl++] = c;
        j++;
    } else {
        while ((raw[j] == '_' || (raw[j] >= 'a' && raw[j] <= 'z') || (raw[j] >= 'A' && raw[j] <= 'Z') || (nl && raw[j] >= '0' && raw[j] <= '9')) && nl < sizeof name - 1)
            name[nl++] = raw[j++];
    }
    name[nl] = 0;
    if (!nl) { put(x, '$', dq ? 'Q' : x->lit); *ip = i + 1; return; }    /* a lone $ */
    *ip = j;
    if (nl == 1 && (name[0] == '@' || name[0] == '*')) { put_params(x, dq, name[0]); return; }
    char *v = param_value(name);
    if (!v && sh.opt_u) { sh_error("%s: parameter not set", name); x->error = 1; return; }
    if (v) put_value(x, v, dq);
    free(v);
}

static void expand_tilde(struct ex *x, const char *raw, size_t len, size_t *ip)
{
    size_t i = *ip + 1, j = i;
    while (j < len && raw[j] != '/' && !((x->flags & EX_ASSIGN) && raw[j] == ':')) j++;
    if (j == i) {                               /* ~ alone: $HOME */
        const char *h = var_get("HOME");
        puts_tag(x, h ? h : "/", 'Q');
        *ip = j;
        return;
    }
    put(x, '~', x->lit);                        /* ~user: no user database here */
    *ip = i;
}

static void expand_into(struct ex *x, const char *raw, size_t len, int dq)
{
    int heredoc = x->flags & EX_HEREDOC;
    for (size_t i = 0; i < len && !x->error;) {
        char c = raw[i];
        if (heredoc) {
            if (c == '\\' && i + 1 < len && (raw[i + 1] == '$' || raw[i + 1] == '`' || raw[i + 1] == '\\' || raw[i + 1] == '\n')) {
                if (raw[i + 1] != '\n') put(x, raw[i + 1], 'Q');
                i += 2;
            } else if (c == '$') do_dollar(x, raw, &i, 1);
            else if (c == '`') { size_t e = skip_backquote(raw, i); do_backquote(x, raw, i + 1, e - 1, 1); i = e; }
            else { put(x, c, 'Q'); i++; }
            continue;
        }
        if (dq) {
            if (c == '"') return;               /* the caller ends the quote */
            if (c == '\\' && i + 1 < len) {
                char d = raw[i + 1];
                if (d == '$' || d == '`' || d == '"' || d == '\\') put(x, d, 'Q');
                else if (d != '\n') { put(x, '\\', 'Q'); put(x, d, 'Q'); }
                i += 2;
                continue;
            }
            if (c == '$') { do_dollar(x, raw, &i, 1); continue; }
            if (c == '`') { size_t e = skip_backquote(raw, i); do_backquote(x, raw, i + 1, e - 1, 1); i = e; continue; }
            put(x, c, 'Q');
            i++;
            continue;
        }
        switch (c) {
        case '\\':
            if (i + 1 < len) put(x, raw[i + 1], 'Q');
            i += 2;
            break;
        case '\'': {
            size_t e = skip_squote(raw, i);
            x->cur.exists = 1;
            for (size_t k = i + 1; k + 1 < e; k++) put(x, raw[k], 'Q');
            i = e;
            break;
        }
        case '"': {
            size_t e = skip_dquote(raw, i);
            /* "$@" with no parameters is no field at all */
            if (!(e - i == 4 && raw[i + 1] == '$' && raw[i + 2] == '@' && params->n == 0)) x->cur.exists = 1;
            expand_into(x, raw + i + 1, e - i - 2, 1);
            i = e;
            break;
        }
        case '$':
            do_dollar(x, raw, &i, 0);
            break;
        case '`': {
            size_t e = skip_backquote(raw, i);
            do_backquote(x, raw, i + 1, e - 1, 0);
            i = e;
            break;
        }
        case '~':
            if (i == 0 || ((x->flags & EX_ASSIGN) && (raw[i - 1] == ':' || raw[i - 1] == '='))) { expand_tilde(x, raw, len, &i); break; }
            /* fall through */
        default:
            put(x, c, x->lit);
            i++;
        }
    }
}

/* ---- splitting and globbing ------------------------------------------------------------- */

static int ifs_has(const char *ifs, char c) { return c && strchr(ifs, c) != NULL; }
static int is_ifs_white(char c) { return c == ' ' || c == '\t' || c == '\n'; }

/* a finished field: a pattern (quoted specials escaped) if it has unquoted glob characters */
struct split { struct str s, m; int has; };

static void glob_field(const char *s, const char *m, size_t n, struct fields *out);

static void emit(struct split *f, struct fields *out, int glob)
{
    if (glob) glob_field(f->s.s ? f->s.s : "", f->m.s ? f->m.s : "", f->s.len, out);
    else fields_add(out, f->s.s ? str_take(&f->s) : xstrdup(""));
    free(f->s.s); free(f->m.s);
    memset(f, 0, sizeof *f);
}

static void split_pre(struct pre *p, int flags, struct fields *out)
{
    int glob = (flags & EX_SPLIT) && !sh.opt_f;
    if (!(flags & EX_SPLIT)) {
        fields_add(out, p->s.s ? xstrdup(p->s.s) : xstrdup(""));
        return;
    }
    if (p->s.len == 0) {
        if (p->exists) fields_add(out, xstrdup(""));
        return;
    }
    const char *ifs = var_get("IFS");
    if (!ifs) ifs = " \t\n";
    struct split f;
    memset(&f, 0, sizeof f);
    int just_white = 0;
    if (p->exists) f.has = 1;
    for (size_t i = 0; i < p->s.len; i++) {
        char c = p->s.s[i], t = p->m.s[i];
        if (t == 'E' && ifs_has(ifs, c)) {
            if (is_ifs_white(c)) {
                if (f.has) { emit(&f, out, glob); just_white = 1; }
            } else {
                if (!(just_white && !f.has)) emit(&f, out, glob);
                just_white = 0;
            }
            continue;
        }
        str_addc(&f.s, c);
        str_addc(&f.m, t);
        f.has = 1;
        just_white = 0;
    }
    if (f.has) emit(&f, out, glob);
    else { free(f.s.s); free(f.m.s); }
}

/* sh pattern matching: * ? [...] (with ! or ^), \x is x */
int pattern_match(const char *p, const char *s)
{
    for (;;) {
        char c = *p;
        if (!c) return *s == 0;
        if (c == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (const char *t = s; ; t++) {
                if (pattern_match(p, t)) return 1;
                if (!*t) return 0;
            }
        }
        if (!*s) return 0;
        if (c == '?') { p++; s++; continue; }
        if (c == '[') {
            const char *q = p + 1;
            int neg = 0;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            int matched = 0, first = 1;
            const char *start = q;
            while (*q && (*q != ']' || first)) {
                first = 0;
                if (*q == '[' && q[1] == ':') {     /* [:class:] */
                    const char *e = strstr(q + 2, ":]");
                    if (e) {
                        char cls[16] = { 0 };
                        size_t cl = (size_t)(e - q - 2) < 15 ? (size_t)(e - q - 2) : 15;
                        memcpy(cls, q + 2, cl);
                        unsigned char ch = (unsigned char)*s;
                        int in =
                            !strcmp(cls, "alpha") ? (ch | 32) >= 'a' && (ch | 32) <= 'z' :
                            !strcmp(cls, "digit") ? ch >= '0' && ch <= '9' :
                            !strcmp(cls, "alnum") ? ((ch | 32) >= 'a' && (ch | 32) <= 'z') || (ch >= '0' && ch <= '9') :
                            !strcmp(cls, "upper") ? ch >= 'A' && ch <= 'Z' :
                            !strcmp(cls, "lower") ? ch >= 'a' && ch <= 'z' :
                            !strcmp(cls, "space") ? ch == ' ' || (ch >= 9 && ch <= 13) :
                            !strcmp(cls, "blank") ? ch == ' ' || ch == '\t' :
                            !strcmp(cls, "punct") ? ch > 32 && ch < 127 && !(((ch | 32) >= 'a' && (ch | 32) <= 'z') || (ch >= '0' && ch <= '9')) :
                            !strcmp(cls, "xdigit") ? (ch >= '0' && ch <= '9') || ((ch | 32) >= 'a' && (ch | 32) <= 'f') :
                            !strcmp(cls, "print") ? ch >= 32 && ch < 127 :
                            !strcmp(cls, "cntrl") ? ch < 32 || ch == 127 : 0;
                        if (in) matched = 1;
                        q = e + 2;
                        continue;
                    }
                }
                char lo = *q;
                if (lo == '\\' && q[1]) lo = *++q;
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    char hi = q[2];
                    if (hi == '\\' && q[3]) { hi = q[3]; q++; }
                    if ((unsigned char)*s >= (unsigned char)lo && (unsigned char)*s <= (unsigned char)hi) matched = 1;
                    q += 3;
                } else {
                    if (*s == lo) matched = 1;
                    q++;
                }
            }
            if (*q != ']') {                    /* no closing bracket: a literal [ */
                (void)start;
                if (*s != '[') return 0;
                p++; s++;
                continue;
            }
            if (matched == neg) return 0;
            p = q + 1;
            s++;
            continue;
        }
        if (c == '\\' && p[1]) { p++; c = *p; }
        if (c != *s) return 0;
        p++; s++;
    }
}

static int has_glob(const char *s, const char *m, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (m[i] != 'Q' && (s[i] == '*' || s[i] == '?' || s[i] == '[')) return 1;
    return 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* expand pattern components [ci..] under dir prefix `base` */
static void glob_walk(const char *base, char **comps, int nc, int ci, struct fields *out)
{
    if (ci == nc) { fields_add(out, xstrdup(base)); return; }
    const char *pat = comps[ci];
    int special = 0;
    for (const char *c = pat; *c; c++) {
        if (*c == '\\' && c[1]) { c++; continue; }
        if (*c == '*' || *c == '?' || *c == '[') { special = 1; break; }
    }
    struct str path = { 0 };
    if (!special) {                             /* a plain component: just check it exists (at the end) */
        str_adds(&path, base);
        if (path.len && path.s[path.len - 1] != '/') str_addc(&path, '/');
        for (const char *c = pat; *c; c++) { if (*c == '\\' && c[1]) c++; str_addc(&path, *c); }
        struct stat st;
        if (ci + 1 == nc ? lstat(path.s, &st) == 0 : (stat(path.s, &st) == 0 && S_ISDIR(st.st_mode)))
            glob_walk(path.s, comps, nc, ci + 1, out);
        free(path.s);
        return;
    }
    DIR *d = opendir(*base ? base : ".");
    if (!d) return;
    struct fields names = { 0 };
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && pat[0] != '.') continue;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { if (strcmp(pat, e->d_name)) continue; }
        if (pattern_match(pat, e->d_name)) fields_add(&names, xstrdup(e->d_name));
    }
    closedir(d);
    if (names.n) qsort(names.v, (size_t)names.n, sizeof *names.v, cmp_str);
    for (int i = 0; i < names.n; i++) {
        struct str p = { 0 };
        str_adds(&p, base);
        if (p.len && p.s[p.len - 1] != '/') str_addc(&p, '/');
        str_adds(&p, names.v[i]);
        if (ci + 1 < nc) {
            struct stat st;
            if (stat(p.s, &st) != 0 || !S_ISDIR(st.st_mode)) { free(p.s); continue; }
        }
        glob_walk(p.s, comps, nc, ci + 1, out);
        free(p.s);
    }
    fields_free(&names);
}

static void glob_field(const char *s, const char *m, size_t n, struct fields *out)
{
    if (!has_glob(s, m, n)) { fields_add(out, xstrndup(s, n)); return; }
    /* the pattern: quoted characters escaped */
    struct str pat = { 0 };
    for (size_t i = 0; i < n; i++) {
        if (m[i] == 'Q' && strchr("*?[]\\", s[i])) str_addc(&pat, '\\');
        str_addc(&pat, s[i]);
    }
    char *ps = pat.s;
    char *comps[256];
    int nc = 0;
    int absolute = ps[0] == '/';
    for (char *t = ps + absolute; *t && nc < 256;) {
        char *e = t;
        while (*e && *e != '/') e++;
        if (e > t) comps[nc++] = xstrndup(t, (size_t)(e - t));
        t = *e ? e + 1 : e;
    }
    int before = out->n;
    glob_walk(absolute ? "/" : "", comps, nc, 0, out);
    for (int i = 0; i < nc; i++) free(comps[i]);
    free(ps);
    if (out->n == before) fields_add(out, xstrndup(s, n));    /* no match: the word as it was */
}

/* ---- entry points ----------------------------------------------------------------------- */

int expand_word(const char *raw, int flags, struct fields *out)
{
    struct ex x;
    memset(&x, 0, sizeof x);
    x.flags = flags;
    x.lit = 'L';
    expand_into(&x, raw, strlen(raw), 0);
    int err = x.error;
    if (!err) {
        if ((flags & EX_PATTERN)) {
            /* one string, quoted pattern characters escaped */
            struct str p = { 0 };
            for (int i = 0; i <= x.npres; i++) {
                struct pre *pr = i < x.npres ? &x.pres[i] : &x.cur;
                if (i) str_addc(&p, ' ');
                for (size_t k = 0; k < pr->s.len; k++) {
                    if (pr->m.s[k] == 'Q' && strchr("*?[]\\", pr->s.s[k])) str_addc(&p, '\\');
                    str_addc(&p, pr->s.s[k]);
                }
            }
            fields_add(out, str_take(&p));
        } else {
            for (int i = 0; i < x.npres; i++) split_pre(&x.pres[i], flags, out);
            if (x.npres == 0 || x.cur.s.len || x.cur.exists) split_pre(&x.cur, flags, out);
        }
    }
    for (int i = 0; i < x.npres; i++) { free(x.pres[i].s.s); free(x.pres[i].m.s); }
    free(x.pres);
    free(x.cur.s.s);
    free(x.cur.m.s);
    if (err == 2 && !sh.interactive) sh_exit(2);
    return err ? -1 : 0;
}

char *expand_one(const char *raw, int flags)
{
    struct fields f = { 0 };
    if (expand_word(raw, flags & ~EX_SPLIT, &f) != 0) { fields_free(&f); return NULL; }
    struct str b = { 0 };
    for (int i = 0; i < f.n; i++) { if (i) str_addc(&b, ' '); str_adds(&b, f.v[i]); }
    fields_free(&f);
    return str_take(&b);
}

int expand_words(const struct words *w, struct fields *out)
{
    for (int i = 0; i < w->n; i++)
        if (expand_word(w->v[i], EX_SPLIT, out) != 0) return -1;
    return 0;
}
