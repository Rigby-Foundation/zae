/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Variables (a hash table; `local` keeps the outer value to put back when
 * the function returns), positional parameters, functions. */
#include "sh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct var {
    struct var *next;
    char *name, *value;         /* value NULL: declared (export x) but unset */
    int flags;
};

#define NBUCKETS 251
static struct var *table[NBUCKETS];

static unsigned hash(const char *s, size_t n)
{
    unsigned h = 5381;
    for (size_t i = 0; i < n; i++) h = h * 33 + (unsigned char)s[i];
    return h % NBUCKETS;
}

static struct var *lookup(const char *name, size_t n, int create)
{
    unsigned h = hash(name, n);
    for (struct var *v = table[h]; v; v = v->next)
        if (strlen(v->name) == n && memcmp(v->name, name, n) == 0) return v;
    if (!create) return NULL;
    struct var *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->name = xstrndup(name, n);
    v->next = table[h];
    table[h] = v;
    return v;
}

int valid_name(const char *s, size_t len)
{
    if (!len || (s[0] >= '0' && s[0] <= '9')) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
    }
    return 1;
}

/* ---- function scopes ---------------------------------------------------------------- */

struct saved { struct saved *next; char *name, *value; int flags, existed; };
struct scope { struct scope *up; struct saved *saved; };
static struct scope *scopes;

void var_push_scope(void)
{
    struct scope *s = xmalloc(sizeof *s);
    s->up = scopes;
    s->saved = NULL;
    scopes = s;
}

void var_pop_scope(void)
{
    struct scope *s = scopes;
    if (!s) return;
    scopes = s->up;
    for (struct saved *k = s->saved, *nx; k; k = nx) {
        nx = k->next;
        struct var *v = lookup(k->name, strlen(k->name), 1);
        free(v->value);
        v->value = k->existed ? k->value : NULL;
        v->flags = k->existed ? k->flags : 0;
        if (!k->existed) free(k->value);
        free(k->name);
        free(k);
    }
    free(s);
}

int var_local(const char *name)
{
    if (!scopes) return -1;
    for (struct saved *k = scopes->saved; k; k = k->next)
        if (strcmp(k->name, name) == 0) return 0;      /* already local here */
    struct var *v = lookup(name, strlen(name), 0);
    struct saved *k = xmalloc(sizeof *k);
    k->name = xstrdup(name);
    k->existed = v && v->value;
    k->value = k->existed ? xstrdup(v->value) : NULL;
    k->flags = v ? v->flags : 0;
    k->next = scopes->saved;
    scopes->saved = k;
    return 0;
}

/* ---- variables ---------------------------------------------------------------------- */

const char *var_get(const char *name)
{
    struct var *v = lookup(name, strlen(name), 0);
    return v ? v->value : NULL;
}

int var_set(const char *name, const char *value, int flags)
{
    struct var *v = lookup(name, strlen(name), 1);
    if (v->flags & V_READONLY) { sh_error("%s: is read only", name); return -1; }
    char *nv = value ? xstrdup(value) : NULL;
    free(v->value);
    v->value = nv;
    v->flags |= flags;
    if (sh.opt_a) v->flags |= V_EXPORT;
    return 0;
}

int var_unset(const char *name)
{
    struct var *v = lookup(name, strlen(name), 0);
    if (!v) return 0;
    if (v->flags & V_READONLY) { sh_error("%s: is read only", name); return -1; }
    free(v->value);
    v->value = NULL;
    v->flags = 0;
    return 0;
}

void var_export(const char *name)
{
    lookup(name, strlen(name), 1)->flags |= V_EXPORT;
}

void var_readonly(const char *name)
{
    lookup(name, strlen(name), 1)->flags |= V_READONLY;
}

int var_flags(const char *name)
{
    struct var *v = lookup(name, strlen(name), 0);
    return v ? v->flags : 0;
}

void vars_init(char **envp)
{
    for (char **e = envp; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq || !valid_name(*e, (size_t)(eq - *e))) continue;
        struct var *v = lookup(*e, (size_t)(eq - *e), 1);
        free(v->value);
        v->value = xstrdup(eq + 1);
        v->flags |= V_EXPORT;
    }
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

char **var_environ(void)
{
    int n = 0;
    for (int i = 0; i < NBUCKETS; i++)
        for (struct var *v = table[i]; v; v = v->next)
            if ((v->flags & V_EXPORT) && v->value) n++;
    char **env = xmalloc(sizeof *env * (size_t)(n + 1));
    int k = 0;
    for (int i = 0; i < NBUCKETS; i++)
        for (struct var *v = table[i]; v; v = v->next)
            if ((v->flags & V_EXPORT) && v->value) {
                size_t ln = strlen(v->name), lv = strlen(v->value);
                char *s = xmalloc(ln + lv + 2);
                memcpy(s, v->name, ln);
                s[ln] = '=';
                memcpy(s + ln + 1, v->value, lv + 1);
                env[k++] = s;
            }
    env[k] = NULL;
    return env;
}

/* set / export -p / readonly -p: name='value', sorted, quoted to be read back */
void var_print(int exported_only, int readonly_only)
{
    int n = 0;
    for (int i = 0; i < NBUCKETS; i++) for (struct var *v = table[i]; v; v = v->next) n++;
    struct var **all = xmalloc(sizeof *all * (size_t)(n + 1));
    int k = 0;
    for (int i = 0; i < NBUCKETS; i++)
        for (struct var *v = table[i]; v; v = v->next) {
            if (exported_only && !(v->flags & V_EXPORT)) continue;
            if (readonly_only && !(v->flags & V_READONLY)) continue;
            if (!exported_only && !readonly_only && !v->value) continue;
            all[k++] = v;
        }
    char **names = xmalloc(sizeof *names * (size_t)(k + 1));
    for (int i = 0; i < k; i++) names[i] = all[i]->name;
    qsort(names, (size_t)k, sizeof *names, cmp_str);
    for (int i = 0; i < k; i++) {
        struct var *v = lookup(names[i], strlen(names[i]), 0);
        if (exported_only) fputs("export ", stdout);
        else if (readonly_only) fputs("readonly ", stdout);
        fputs(v->name, stdout);
        if (v->value) {
            fputs("='", stdout);
            for (const char *c = v->value; *c; c++) {
                if (*c == '\'') fputs("'\\''", stdout);
                else putchar(*c);
            }
            putchar('\'');
        }
        putchar('\n');
    }
    free(names);
    free(all);
}

/* ---- positional parameters ----------------------------------------------------------- */

static struct params top;
struct params *params = &top;
char *arg0;

static void params_fill(struct params *p, char **v, int n)
{
    p->v = xmalloc(sizeof *p->v * (size_t)(n + 1));
    for (int i = 0; i < n; i++) p->v[i] = xstrdup(v[i]);
    p->v[n] = NULL;
    p->n = n;
}

void params_set(char **v, int n)
{
    struct params old = *params;
    params_fill(params, v, n);                  /* v may point into the old ones */
    for (int i = 0; i < old.n; i++) free(old.v[i]);
    free(old.v);
}

struct params *params_push(char **v, int n)
{
    struct params *saved = params;
    struct params *p = xmalloc(sizeof *p);
    params_fill(p, v, n);
    params = p;
    return saved;
}

void params_pop(struct params *saved)
{
    for (int i = 0; i < params->n; i++) free(params->v[i]);
    free(params->v);
    if (params != &top) free(params);
    params = saved;
}

/* ---- functions ---------------------------------------------------------------------- */

struct func { struct func *next; char *name; struct node *body; };
static struct func *funcs;

struct node *func_get(const char *name)
{
    for (struct func *f = funcs; f; f = f->next) if (strcmp(f->name, name) == 0) return f->body;
    return NULL;
}

void func_set(const char *name, struct node *body)
{
    for (struct func *f = funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) { f->body = body; return; }
    struct func *f = xmalloc(sizeof *f);
    f->name = xstrdup(name);
    f->body = body;
    f->next = funcs;
    funcs = f;
}

int func_unset(const char *name)
{
    for (struct func **pp = &funcs; *pp; pp = &(*pp)->next)
        if (strcmp((*pp)->name, name) == 0) {
            struct func *f = *pp;
            *pp = f->next;
            free(f->name);
            free(f);
            return 0;
        }
    return 1;
}
