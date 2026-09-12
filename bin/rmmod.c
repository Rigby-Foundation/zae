/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: rmmod <name>\n"); return 2; }
    if (syscall(SYS_delete_module, argv[1], 0) != 0) {
        fprintf(stderr, "rmmod: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
