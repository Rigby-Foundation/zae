/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Allocation that cannot fail (the shell has nothing better to do than
 * stop), growable strings, error messages. */
#include "sh.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("sh: out of memory\n", stderr); _Exit(2); }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { fputs("sh: out of memory\n", stderr); _Exit(2); }
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void sh_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fflush(stdout);
    fputs("sh: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void str_grow(struct str *b, size_t more)
{
    if (b->len + more + 1 > b->cap) {
        b->cap = (b->len + more + 1) * 2;
        if (b->cap < 32) b->cap = 32;
        b->s = xrealloc(b->s, b->cap);
    }
}

void str_addc(struct str *b, char c) { str_grow(b, 1); b->s[b->len++] = c; b->s[b->len] = 0; }

void str_addn(struct str *b, const char *s, size_t n)
{
    str_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void str_adds(struct str *b, const char *s) { str_addn(b, s, strlen(s)); }

char *str_take(struct str *b)
{
    char *s = b->s ? b->s : xstrdup("");
    b->s = NULL;
    b->len = b->cap = 0;
    return s;
}
