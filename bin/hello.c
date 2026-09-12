/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

int main(int argc, char **argv)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("hello from pid %d on cpu %d at %ld.%03ld s, argc=%d:", getpid(), sched_getcpu(),
           (long)ts.tv_sec, ts.tv_nsec / 1000000, argc);
    for (int i = 0; i < argc; i++)
        printf(" [%s]", argv[i]);
    printf("\n");

    /* exercise the heap and floating point, both new with musl */
    double *v = malloc(1000 * sizeof(double));
    double sum = 0;
    for (int i = 0; i < 1000; i++) { v[i] = i * 0.5; sum += v[i]; }
    printf("malloc + sse ok: sum=%.1f\n", sum);
    free(v);
    return 0;
}
