/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <ctype.h>

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f) { perror(argv[1]); return 1; }
    long lines = 0, words = 0, chars = 0;
    int c, inword = 0;
    while ((c = fgetc(f)) != EOF) {
        chars++;
        if (c == '\n') lines++;
        if (isspace(c)) inword = 0;
        else if (!inword) { inword = 1; words++; }
    }
    printf("%7ld %7ld %7ld%s%s\n", lines, words, chars, argc > 1 ? " " : "", argc > 1 ? argv[1] : "");
    return 0;
}
