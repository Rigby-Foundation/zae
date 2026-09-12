/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>

static int dump(FILE *f)
{
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return dump(stdin);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            perror(argv[i]);
            rc = 1;
            continue;
        }
        dump(f);
        fclose(f);
    }
    return rc;
}
