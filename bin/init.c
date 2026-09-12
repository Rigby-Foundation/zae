/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* PID 1: prints the motd, then keeps a shell running. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>

int main(void)
{
    FILE *f = fopen("/etc/motd", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f))
            fputs(line, stdout);
        fclose(f);
    }
    setenv("PATH", "/bin", 1);
    setenv("HOME", "/", 1);

    /* Persistent storage: mount the zaefs disk on /disk, formatting it first if needed. */
    struct stat st;
    if (stat("/dev/nvme0n1", &st) == 0) {
        if (mount("/dev/nvme0n1", "/disk", "zaefs", 0, NULL) != 0 && errno != EBUSY) {
            printf("init: /dev/nvme0n1 has no zaefs, formatting\n");
            pid_t pid = fork();
            if (pid == 0) {
                execl("/bin/mkfs.zaefs", "mkfs.zaefs", "-L", "sicdisk", "/dev/nvme0n1", (char *)NULL);
                _exit(127);
            }
            int st2;
            waitpid(pid, &st2, 0);
            if (mount("/dev/nvme0n1", "/disk", "zaefs", 0, NULL) != 0)
                printf("init: mount /disk failed: %s\n", strerror(errno));
        }
    }

    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/sh", "sh", (char *)NULL);
            perror("init: exec /bin/sh");
            _exit(1);
        }
        if (pid < 0) {
            perror("init: fork");
            sleep(1);
            continue;
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
            printf("init: shell exited (%d), restarting\n", WEXITSTATUS(status));
        else
            printf("init: shell killed by signal %d, restarting\n", WTERMSIG(status));
    }
}
