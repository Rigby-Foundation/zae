/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>

int main(int argc, char **argv)
{
    const char *type = "zaefs", *src = NULL, *dst = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) type = argv[++i];
        else if (!src) src = argv[i];
        else dst = argv[i];
    }
    if (!src || !dst) {
        fprintf(stderr, "usage: mount [-t type] <device> <dir>\n");
        return 2;
    }
    if (mount(src, dst, type, 0, NULL) != 0) {
        fprintf(stderr, "mount: %s on %s: %s\n", src, dst, strerror(errno));
        return 1;
    }
    return 0;
}
