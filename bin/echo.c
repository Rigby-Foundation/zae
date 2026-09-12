/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <string.h>

/* echo [-n] words...   or   echo words > file  (a tiny redirect; the shell has none yet) */
int main(int argc, char **argv)
{
    FILE *out = stdout;
    int start = 1, nl = 1, end = argc;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { nl = 0; start = 2; }
    if (argc >= 3 && strcmp(argv[argc - 2], ">") == 0) {
        out = fopen(argv[argc - 1], "w");
        if (!out) { perror(argv[argc - 1]); return 1; }
        end = argc - 2;
    }
    for (int i = start; i < end; i++)
        fprintf(out, "%s%s", argv[i], i + 1 < end ? " " : "");
    if (nl)
        fputc('\n', out);
    if (out != stdout)
        fclose(out);
    return 0;
}
