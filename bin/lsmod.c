/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    char buf[4096];
    long n = syscall(SYS_query_module, buf, sizeof(buf));
    if (n < 0) { perror("lsmod"); return 1; }
    printf("Module     Base        Size\n");
    fwrite(buf, 1, n, stdout);
    return 0;
}
