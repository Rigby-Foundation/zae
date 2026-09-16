/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int sig = SIGTERM, i = 1;
    if (argc > 1 && argv[1][0] == '-') { sig = atoi(argv[1] + 1); i = 2; }
    if (i >= argc) { fprintf(stderr, "usage: kill [-signal] pid...\n"); return 2; }
    int rc = 0;
    for (; i < argc; i++)
        if (kill(atoi(argv[i]), sig) != 0) { fprintf(stderr, "kill: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
    return rc;
}
