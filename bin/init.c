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
#include <dirent.h>

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

    /* Persistent storage: the first zaefs volume found goes on /disk (an
     * installed system's root partition, or a whole disk used as scratch);
     * any further ones (a game disk made with mkzaefs.py) on /mnt/<device>. */
    {
        DIR *d = opendir("/dev");
        struct dirent *e;
        int mounted = 0;
        while (d && (e = readdir(d))) {
            char path[64], where[80];
            struct stat st;
            snprintf(path, sizeof(path), "/dev/%s", e->d_name);
            if (strcmp(e->d_name, "initrd") == 0 || stat(path, &st) != 0 || !S_ISBLK(st.st_mode)) continue;
            if (!mounted && mount(path, "/disk", "zaefs", 0, NULL) == 0) {
                printf("init: mounted %s on /disk\n", path);
                mounted++;
                continue;
            }
            if (!mounted && errno == EBUSY)
                mounted++;                  /* the kernel's self test left a volume on /disk; this may or may not be it */
            if (!mounted) continue;         /* not a zaefs volume */
            snprintf(where, sizeof(where), "/mnt/%s", e->d_name);
            mkdir("/mnt", 0755); mkdir(where, 0755);
            if (mount(path, where, "zaefs", 0, NULL) == 0) {
                printf("init: mounted %s on %s\n", path, where);
                mounted++;
            } else
                rmdir(where);               /* the one on /disk, or no volume */
        }
        if (d) closedir(d);
        if (!mounted)
            printf("init: no zaefs volume found; nothing mounted on /disk\n");
    }

    /* Networking: lease an address on the first Ethernet interface, in the
     * background so a cable-less machine doesn't hold the shell up. */
    if (access("/bin/dhcp", X_OK) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/dhcp", "dhcp", "-t", "20", (char *)NULL);
            _exit(1);
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
        for (;;) {                  /* reap background helpers (dhcp) too */
            pid_t w = waitpid(-1, &status, 0);
            if (w == pid || (w < 0 && errno != EINTR)) break;
        }
        if (WIFEXITED(status))
            printf("init: shell exited (%d), restarting\n", WEXITSTATUS(status));
        else
            printf("init: shell killed by signal %d, restarting\n", WTERMSIG(status));
    }
}
