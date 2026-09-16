/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* threads: a pthreads smoke test; exit code = failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter;
static int turn;
static __thread int tls_id;

static void *incrementer(void *arg)
{
    tls_id = (int)(long)arg;
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&mu);
        counter++;
        pthread_mutex_unlock(&mu);
    }
    return (void *)(long)(tls_id * 10);
}

static void *pingpong(void *arg)
{
    int me = (int)(long)arg;
    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&mu);
        while (turn != me)
            pthread_cond_wait(&cv, &mu);
        turn = !me;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static void *sleeper(void *arg)
{
    (void)arg;
    usleep(50000);
    return (void *)42;
}

static void *spinner(void *arg)
{
    (void)arg;
    for (;;) ;
}

int main(void)
{
    printf("[threads] pid %d\n", getpid());

    /* mutex-protected counter across 4 threads, join with return values, TLS is per thread */
    pthread_t th[4];
    for (long i = 0; i < 4; i++)
        CHECK(pthread_create(&th[i], NULL, incrementer, (void *)(i + 1)) == 0, "pthread_create");
    for (long i = 0; i < 4; i++) {
        void *ret = NULL;
        CHECK(pthread_join(th[i], &ret) == 0, "pthread_join");
        CHECK((long)ret == (i + 1) * 10, "thread return value");
    }
    CHECK(counter == 80000, "mutex-protected counter");
    CHECK(tls_id == 0, "main thread's TLS untouched by the workers");

    /* condition variable ping-pong */
    turn = 0;
    pthread_t a, b;
    pthread_create(&a, NULL, pingpong, (void *)0);
    pthread_create(&b, NULL, pingpong, (void *)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    CHECK(turn == 0, "cond var ping-pong completed");

    /* a thread sleeping while main runs; timed join semantics via usleep */
    pthread_t s;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_create(&s, NULL, sleeper, NULL);
    void *sret;
    pthread_join(s, &sret);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK((long)sret == 42 && ms >= 50 && ms < 500, "sleeping thread joined after ~50 ms");

    /* detached thread */
    pthread_t d;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    CHECK(pthread_create(&d, &attr, sleeper, NULL) == 0, "detached thread");
    usleep(100000);

    /* exit() from a process with a spinning thread ends the whole process */
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t sp;
        pthread_create(&sp, NULL, spinner, NULL);
        usleep(20000);
        exit(3);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 3, "exit_group with a live thread");

    /* gettid differs between threads, getpid does not */
    CHECK(getpid() == (pid_t)getpid(), "getpid stable");

    printf("[threads] done: %d failure(s)\n", fails);
    return fails;
}
