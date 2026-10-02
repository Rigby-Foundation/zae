/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* menu: a session for a machine with three buttons and no keyboard (a
 * phone: the volume keys are up and down, power is Enter). A list of
 * things to run; the arrows move, Enter runs. Name it in /etc/session. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <abi/fb.h>

enum { RUN, KMSG, REBOOT, POWEROFF, DESKTOP };
static const struct { const char *label; int kind; const char *argv[4]; } items[] = {
    { "The desktop (touch)",       DESKTOP,  { "/bin/zde", NULL } },
    { "About this machine",        RUN,      { "/bin/uname", "-a", NULL } },
    { "What is in /bin",           RUN,      { "/bin/ls", "/bin", NULL } },
    { "The kernel log (the end)",  KMSG,     { NULL } },
    { "The self test",             RUN,      { "/bin/test", NULL } },
    { "Threads",                   RUN,      { "/bin/threads", NULL } },
    { "Restart",                   REBOOT,   { NULL } },
    { "Power off",                 POWEROFF, { NULL } },
};
#define N (int)(sizeof items / sizeof items[0])

static int con = -1;

/* The next key: 'u', 'd' or '\n' (presses only; releases and the rest are skipped). */
static int next_key(void)
{
    int ext = 0;
    for (;;) {
        unsigned char c;
        if (read(con, &c, 1) != 1) { usleep(20000); continue; }
        if (c == 0xe0) { ext = 1; continue; }
        int e = ext;
        ext = 0;
        if (c & 0x80) continue;                         /* a release */
        if (c == 0x1c) return '\n';
        if (e && c == 0x48) return 'u';
        if (e && c == 0x50) return 'd';
    }
}

static void draw(int sel)
{
    printf("\n--- sic ------------------------------------------------\n");
    for (int i = 0; i < N; i++)
        printf("  %s %s\n", i == sel ? "[>]" : "   ", items[i].label);
    printf("volume keys: move, power: run\n");
    fflush(stdout);
}

static void kmsg_tail(int lines)
{
    static char buf[65536];
    int fd = open("/proc/kmsg", O_RDONLY), n = 0, r;
    if (fd < 0) { perror("/proc/kmsg"); return; }
    while (n < (int)sizeof buf - 1 && (r = (int)read(fd, buf + n, sizeof buf - 1 - (size_t)n)) > 0) n += r;
    close(fd);
    buf[n] = 0;
    char *p = buf + n;
    for (int seen = 0; p > buf; p--)
        if (p[-1] == '\n' && ++seen > lines) break;
    fputs(p, stdout);
}

int main(void)
{
    con = open("/dev/console", O_RDONLY | O_NONBLOCK);
    if (con < 0 || ioctl(con, KDSKBMODE, K_RAW) != 0) {
        perror("menu: /dev/console raw mode");
        execl("/bin/sh", "sh", (char *)NULL);
        return 1;
    }
    int sel = 0;
    draw(sel);
    for (;;) {
        int k = next_key();
        if (k == 'u' || k == 'd') {
            sel = (sel + (k == 'u' ? N - 1 : 1)) % N;
            draw(sel);
            continue;
        }
        printf("\n> %s\n", items[sel].label);
        fflush(stdout);
        switch (items[sel].kind) {
        case RUN: {
            pid_t pid = fork();
            if (pid == 0) { execv(items[sel].argv[0], (char *const *)items[sel].argv); perror(items[sel].argv[0]); _exit(127); }
            int st;
            if (pid > 0) waitpid(pid, &st, 0);
            break;
        }
        case KMSG: kmsg_tail(40); break;
        case DESKTOP: {                         /* zwm takes the console's keys while it runs */
            ioctl(con, KDSKBMODE, K_XLATE);        /* let go: raw mode has one owner */
            pid_t pid = fork();
            if (pid == 0) { setenv("ZWM_SCALE", "2", 1); execv(items[sel].argv[0], (char *const *)items[sel].argv); perror(items[sel].argv[0]); _exit(127); }
            int st;
            if (pid > 0) waitpid(pid, &st, 0);
            ioctl(con, KDSKBMODE, K_RAW);          /* ours again */
            break;
        }
        case REBOOT: sync(); reboot(RB_AUTOBOOT); break;
        case POWEROFF: sync(); reboot(RB_POWER_OFF); break;
        }
        draw(sel);
    }
}
