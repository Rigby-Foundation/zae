/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* forkstress [n]: fork n children (default 2000) as fast as possible and
 * check each one sees fork() == 0 and gets back to the parent with the
 * right status. For SMP bugs in the context switch and fork paths. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>

/* forkstress -p [n]: batches of 8 children running while the parent keeps
 * forking and sleeping, then collects them. */
static int parallel(int n)
{
    int bad = 0;
    pid_t me = getpid();
    for (int i = 0; i < n; i += 8) {
        pid_t kids[8];
        for (int k = 0; k < 8; k++) {
            volatile long canary = 0x77770000L + i + k;
            pid_t p = fork();
            if (p == 0) {
                if (getpid() == me) _exit(99);
                for (volatile int spin = 0; spin < 20000; spin++) ;
                usleep(100);
                _exit(canary == 0x77770000L + i + k ? k : 100);
            }
            kids[k] = p;
            usleep(50);
        }
        for (int k = 0; k < 8; k++) {
            int st;
            if (waitpid(kids[k], &st, 0) != kids[k] || !WIFEXITED(st) || WEXITSTATUS(st) != k) {
                printf("forkstress: batch %d child %d: %s %d\n", i, k, WIFEXITED(st) ? "exited" : "killed by", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
                if (++bad > 10) return bad;
            }
        }
    }
    return bad;
}

/* forkstress -k [n]: fork a child that execs /bin/echo and kill it right
 * away (the SIGTERM lands between fork and exec, or inside exec), then
 * check a plain fork still works. */
static int killtest(int n)
{
    int bad = 0;
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0) { execl("/bin/echo", "echo", "-n", "", (char *)NULL); _exit(127); }
        if (i & 1) usleep(i % 7 * 100);
        kill(p, SIGTERM);
        int st;
        waitpid(p, &st, 0);
        volatile long canary = 0x6b6b0000L + i;
        pid_t q = fork();
        if (q == 0) _exit(canary == 0x6b6b0000L + i ? 7 : 100);
        if (waitpid(q, &st, 0) != q || !WIFEXITED(st) || WEXITSTATUS(st) != 7) {
            printf("forkstress: kill round %d: fork after kill went wrong (%s %d)\n", i, WIFEXITED(st) ? "exit" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
            if (++bad > 10) break;
        }
    }
    return bad;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-k") == 0) {
        int n = argc > 2 ? atoi(argv[2]) : 1000, bad = killtest(n);
        printf("forkstress: %d kill rounds, %d bad\n", n, bad);
        return bad != 0;
    }
    if (argc > 1 && argv[1][0] == '-') {
        int n = argc > 2 ? atoi(argv[2]) : 2000, bad = parallel(n);
        printf("forkstress: %d parallel forks, %d bad\n", n, bad);
        return bad != 0;
    }
    int n = argc > 1 ? atoi(argv[1]) : 2000, bad = 0;
    pid_t me = getpid();
    for (int i = 0; i < n; i++) {
        volatile long canary = 0x5a5a0000L + i;
        pid_t p = fork();
        if (p < 0) { perror("fork"); return 1; }
        if (p == 0) {
            if (getpid() == me) _exit(99);          /* can't be: we are the parent */
            _exit(canary == 0x5a5a0000L + i ? i & 0x7f : 100);
        }
        int st;
        if (waitpid(p, &st, 0) != p) { printf("forkstress: %d: waitpid failed\n", i); bad++; continue; }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != (i & 0x7f)) {
            printf("forkstress: %d: child %d %s %d\n", i, p, WIFEXITED(st) ? "exited" : "killed by", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
            if (++bad > 10) break;
        }
    }
    printf("forkstress: %d forks, %d bad\n", n, bad);
    return bad != 0;
}
