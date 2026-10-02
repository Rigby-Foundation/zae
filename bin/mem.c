/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* mem: look at and poke hardware registers through /dev/physmem.
 *   mem ADDR [COUNT]       COUNT 32-bit words from ADDR (hex)
 *   mem ADDR = VALUE       write one word
 *   mem ADDR |= BITS       set bits;  mem ADDR &= MASK: keep bits */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

static int fd;

static uint32_t peek(uint64_t a)
{
    uint32_t v = 0;
    if (lseek(fd, (off_t)a, SEEK_SET) < 0 || read(fd, &v, 4) != 4) { perror("read"); exit(1); }
    return v;
}

static void poke(uint64_t a, uint32_t v)
{
    if (lseek(fd, (off_t)a, SEEK_SET) < 0 || write(fd, &v, 4) != 4) { perror("write"); exit(1); }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: mem ADDR [COUNT] | ADDR = V | ADDR |= V | ADDR &= V\n"); return 2; }
    fd = open("/dev/physmem", O_RDWR);
    if (fd < 0) { perror("/dev/physmem"); return 1; }
    uint64_t a = strtoull(argv[1], NULL, 16) & ~3ULL;
    if (argc == 4) {
        uint32_t v = (uint32_t)strtoul(argv[3], NULL, 16), old = peek(a);
        if (!strcmp(argv[2], "=")) poke(a, v);
        else if (!strcmp(argv[2], "|=")) poke(a, old | v);
        else if (!strcmp(argv[2], "&=")) poke(a, old & v);
        else { fprintf(stderr, "mem: = |= or &=\n"); return 2; }
        printf("%08llx: %08x -> %08x\n", (unsigned long long)a, old, peek(a));
        return 0;
    }
    int n = argc > 2 ? atoi(argv[2]) : 1;
    for (int i = 0; i < n; i++) {
        if (i % 4 == 0) printf("%s%08llx:", i ? "\n" : "", (unsigned long long)(a + 4 * (uint64_t)i));
        printf(" %08x", peek(a + 4 * (uint64_t)i));
    }
    printf("\n");
    return 0;
}
