/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: umount <dir>\n"); return 2; }
    if (umount2(argv[1], 0) != 0) {
        fprintf(stderr, "umount: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
