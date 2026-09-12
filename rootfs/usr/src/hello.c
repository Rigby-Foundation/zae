/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Try it on sic:   tcc /usr/src/hello.c -o /tmp/hello && /tmp/hello */
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    printf("hello from a program compiled on sic itself (pid %d)\n", getpid());
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);
    return 0;
}
