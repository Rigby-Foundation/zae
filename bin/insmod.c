/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: insmod <module.ko> [params]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *img = malloc(len);
    if (!img || fread(img, 1, len, f) != (size_t)len) { perror("read"); return 1; }
    fclose(f);
    if (syscall(SYS_init_module, img, len, argc > 2 ? argv[2] : "") != 0) {
        fprintf(stderr, "insmod: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
