/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Misbehaving program: must be killed without taking the kernel down. */
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("[crash] pid %d: about to touch kernel memory at 0x100000\n", getpid());
    fflush(stdout);
    volatile unsigned long *kernel = (unsigned long *)0x100000;
    unsigned long v = *kernel;                 /* not user-accessible -> SIGSEGV */
    printf("[crash] read %lx, this should not print\n", v);
    return 0;
}
