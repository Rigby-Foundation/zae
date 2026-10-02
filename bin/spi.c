/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* spi: raw transfers on /dev/spi0, for finding out how a device talks.
 *   spi w BYTES...     send (hex bytes)
 *   spi r N            clock in N bytes, print them
 *   spi wr N BYTES...  send, then clock in N bytes (two transfers) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void show(const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) printf("%02x%s", b[i], (i % 32 == 31 || i == n - 1) ? "\n" : " ");
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: spi w BYTES... | r N | wr N BYTES...\n"); return 2; }
    int fd = open("/dev/spi0", O_RDWR);
    if (fd < 0) { perror("/dev/spi0"); return 1; }
    static unsigned char buf[4096];
    int first = strcmp(argv[1], "wr") == 0 ? 3 : 2;
    if (argv[1][0] == 'w') {
        int n = 0;
        for (int i = first; i < argc && n < (int)sizeof buf; i++) buf[n++] = (unsigned char)strtoul(argv[i], NULL, 16);
        if (write(fd, buf, (size_t)n) != n) { perror("write"); return 1; }
    }
    if (argv[1][0] == 'r' || strcmp(argv[1], "wr") == 0) {
        int n = atoi(argv[2]);
        if (n <= 0 || n > (int)sizeof buf) n = 64;
        if (read(fd, buf, (size_t)n) != n) { perror("read"); return 1; }
        show(buf, n);
    }
    return 0;
}
