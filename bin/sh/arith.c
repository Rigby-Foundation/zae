/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* $((...)): C's integer expressions on longs (XCU 2.6.4): variables by
 * name (their value read as a number, empty or unset as 0), the assignment
 * operators, ++ and --, ?:, and && || that skip what they need not
 * evaluate. The text has been through parameter expansion already. */
#include "sh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ar {
    const char *s;
    int err;
    int skip;                   /* inside the side of && || ?: that does not count: no assignments */
};

static void ws(struct ar *a) { while (*a->s == ' ' || *a->s == '\t' || *a->s == '\n') a->s++; }

static void fail(struct ar *a, const char *what)
{
    if (!a->err) sh_error("arithmetic: %s near '%s'", what, a->s);
    a->err = 1;
}

static long var_num(const char *name)
{
    const char *v = var_get(name);
    if (!v || !*v) return 0;
    int err = 0;
    long r = arith_eval(v, &err);               /* a variable may hold an expression */
    return err ? 0 : r;
}

static void var_num_set(struct ar *a, const char *name, long v)
{
    if (a->skip) return;
    char num[32];
    snprintf(num, sizeof num, "%ld", v);
    var_set(name, num, 0);
}

static long assign(struct ar *a);

static int name_here(struct ar *a, char *name, size_t cap)
{
    const char *s = a->s;
    size_t n = 0;
    if (!(*s == '_' || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) return 0;
    while ((*s == '_' || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')) && n < cap - 1) name[n++] = *s++;
    name[n] = 0;
    a->s = s;
    return 1;
}

static long primary(struct ar *a)
{
    ws(a);
    char c = *a->s;
    if (c == '(') {
        a->s++;
        long v = assign(a);
        ws(a);
        if (*a->s != ')') { fail(a, "expected )"); return 0; }
        a->s++;
        return v;
    }
    if (c >= '0' && c <= '9') {
        char *end;
        long v;
        if (c == '0' && (a->s[1] == 'x' || a->s[1] == 'X')) v = strtol(a->s + 2, &end, 16);
        else if (c == '0') v = strtol(a->s, &end, 8);
        else v = strtol(a->s, &end, 10);
        a->s = end;
        return v;
    }
    char name[128];
    if (name_here(a, name, sizeof name)) {
        ws(a);
        if ((a->s[0] == '+' && a->s[1] == '+') || (a->s[0] == '-' && a->s[1] == '-')) {   /* x++ x-- */
            int inc = a->s[0] == '+';
            a->s += 2;
            long v = var_num(name);
            var_num_set(a, name, inc ? v + 1 : v - 1);
            return v;
        }
        return var_num(name);
    }
    fail(a, "expected a number");
    return 0;
}

static long unary(struct ar *a)
{
    ws(a);
    char c = *a->s;
    if ((c == '+' || c == '-') && a->s[1] == c) {   /* ++x --x */
        a->s += 2;
        ws(a);
        char name[128];
        if (!name_here(a, name, sizeof name)) { fail(a, "++/-- needs a variable"); return 0; }
        long v = var_num(name) + (c == '+' ? 1 : -1);
        var_num_set(a, name, v);
        return v;
    }
    if (c == '-') { a->s++; return -unary(a); }
    if (c == '+') { a->s++; return unary(a); }
    if (c == '!') { a->s++; return !unary(a); }
    if (c == '~') { a->s++; return ~unary(a); }
    return primary(a);
}

/* binary operators by precedence, lowest last */
static const struct { const char *op; int prec; } ops[] = {
    { "*", 10 }, { "/", 10 }, { "%", 10 },
    { "+", 9 }, { "-", 9 },
    { "<<", 8 }, { ">>", 8 },
    { "<=", 7 }, { ">=", 7 }, { "<", 7 }, { ">", 7 },
    { "==", 6 }, { "!=", 6 },
    { "&", 5 }, { "^", 4 }, { "|", 3 },
    { "&&", 2 }, { "||", 1 },
};

static int peek_op(struct ar *a, int *prec)
{
    ws(a);
    int best = -1;
    size_t bestlen = 0;
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        size_t l = strlen(ops[i].op);
        if (strncmp(a->s, ops[i].op, l) == 0 && l > bestlen) {
            /* not an assignment operator (a+=1, a<<=1, a==1 is fine) */
            if (a->s[l] == '=' && strcmp(ops[i].op, "<") && strcmp(ops[i].op, ">") && strcmp(ops[i].op, "=") && strcmp(ops[i].op, "!")) continue;
            best = (int)i;
            bestlen = l;
        }
    }
    if (best < 0) return -1;
    *prec = ops[best].prec;
    return best;
}

static long binary(struct ar *a, int min_prec)
{
    long left = unary(a);
    for (;;) {
        int prec;
        int k = peek_op(a, &prec);
        if (k < 0 || prec < min_prec) return left;
        const char *op = ops[k].op;
        a->s += strlen(op);
        if (!strcmp(op, "&&") || !strcmp(op, "||")) {
            int and = op[0] == '&';
            int skip_right = and ? !left : !!left;
            a->skip += skip_right;
            long right = binary(a, prec + 1);
            a->skip -= skip_right;
            left = and ? (left && right) : (left || right);
            continue;
        }
        long right = binary(a, prec + 1);
        if (a->err) return 0;
        switch (op[0]) {
        case '*': left *= right; break;
        case '/': case '%':
            if (right == 0) { if (!a->skip) fail(a, "division by zero"); return 0; }
            left = op[0] == '/' ? left / right : left % right;
            break;
        case '+': left += right; break;
        case '-': left -= right; break;
        case '<': left = op[1] == '<' ? left << right : op[1] == '=' ? left <= right : left < right; break;
        case '>': left = op[1] == '>' ? left >> right : op[1] == '=' ? left >= right : left > right; break;
        case '=': left = left == right; break;
        case '!': left = left != right; break;
        case '&': left &= right; break;
        case '^': left ^= right; break;
        case '|': left |= right; break;
        }
    }
}

static long ternary(struct ar *a)
{
    long c = binary(a, 1);
    ws(a);
    if (*a->s != '?') return c;
    a->s++;
    a->skip += !c;
    long t = assign(a);
    a->skip -= !c;
    ws(a);
    if (*a->s != ':') { fail(a, "expected :"); return 0; }
    a->s++;
    a->skip += !!c;
    long f = ternary(a);
    a->skip -= !!c;
    return c ? t : f;
}

static long assign(struct ar *a)
{
    ws(a);
    const char *save = a->s;
    char name[128];
    if (name_here(a, name, sizeof name)) {
        ws(a);
        static const char *const aops[] = { "<<=", ">>=", "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "=", NULL };
        for (int i = 0; aops[i]; i++) {
            size_t l = strlen(aops[i]);
            if (strncmp(a->s, aops[i], l) == 0 && !(l == 1 && a->s[1] == '=')) {
                a->s += l;
                long r = assign(a), v = var_num(name);
                switch (aops[i][0]) {
                case '=': v = r; break;
                case '*': v *= r; break;
                case '/': case '%':
                    if (r == 0) { if (!a->skip) fail(a, "division by zero"); return 0; }
                    v = aops[i][0] == '/' ? v / r : v % r;
                    break;
                case '+': v += r; break;
                case '-': v -= r; break;
                case '&': v &= r; break;
                case '^': v ^= r; break;
                case '|': v |= r; break;
                case '<': v <<= r; break;
                case '>': v >>= r; break;
                }
                var_num_set(a, name, v);
                return v;
            }
        }
        a->s = save;
    }
    return ternary(a);
}

long arith_eval(const char *expr, int *err)
{
    struct ar a = { expr, 0, 0 };
    ws(&a);
    if (!*a.s) { *err = 0; return 0; }
    long v = assign(&a);
    ws(&a);
    while (!a.err && *a.s == ',') {             /* a, b: both, the value is b's (not POSIX; bash and dash have it) */
        a.s++;
        v = assign(&a);
        ws(&a);
    }
    if (!a.err && *a.s) fail(&a, "unexpected text");
    *err = a.err;
    return a.err ? 0 : v;
}
