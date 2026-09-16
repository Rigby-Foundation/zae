/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* grep: fixed-string search, files or stdin. */
#include <stdio.h>
#include <string.h>

static int search(const char *pat, FILE *f, const char *name, int show)
{
    char line[1024];
    int hits = 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, pat)) {
            if (show) printf("%s:", name);
            fputs(line, stdout);
            hits++;
        }
    return hits;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: grep <string> [files...]\n"); return 2; }
    int hits = 0;
    if (argc == 2)
        hits = search(argv[1], stdin, "-", 0);
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) { perror(argv[i]); continue; }
        hits += search(argv[1], f, argv[i], argc > 3);
        fclose(f);
    }
    return hits ? 0 : 1;
}
