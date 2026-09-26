/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* tcpstress [n]: the self test's TCP-over-loopback exchange, n times
 * (default 200): a forked client sends 100000 bytes and half-closes, the
 * server checks them, answers "thanks" after the client's FIN, closes, and
 * a connect to a closed port must be refused. Prints a line every 50
 * rounds, so a hang shows where it stopped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int client(void)
{
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ca = { .sin_family = AF_INET, .sin_port = htons(8088), .sin_addr.s_addr = htonl(0x7F000001) };
    if (connect(c, (struct sockaddr *)&ca, sizeof ca) != 0) return 1;
    static char big[100000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7);
    for (size_t off = 0; off < sizeof big; ) {
        ssize_t w = send(c, big + off, sizeof big - off, 0);
        if (w <= 0) return 2;
        off += (size_t)w;
    }
    shutdown(c, SHUT_WR);
    char echo[16];
    if (recv(c, echo, sizeof echo, 0) != 6 || memcmp(echo, "thanks", 6) != 0) return 3;
    if (recv(c, echo, sizeof echo, 0) != 0) return 4;
    close(c);
    return 0;
}

static int round_(void)
{
    int l = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in la = { .sin_family = AF_INET, .sin_port = htons(8088), .sin_addr.s_addr = INADDR_ANY };
    if (bind(l, (struct sockaddr *)&la, sizeof la) != 0 || listen(l, 4) != 0) { perror("tcpstress: bind/listen"); return 1; }
    pid_t child = fork();
    if (child == 0) _exit(client());
    int c = accept(l, NULL, NULL);
    if (c < 0) { perror("tcpstress: accept"); return 1; }
    size_t total = 0;
    int ok = 1;
    for (;;) {
        char rb[4096];
        ssize_t r = recv(c, rb, sizeof rb, 0);
        if (r < 0) { ok = 0; break; }
        if (r == 0) break;
        for (ssize_t i = 0; i < r; i++)
            if (rb[i] != (char)((total + (size_t)i) * 7)) ok = 0;
        total += (size_t)r;
    }
    if (!ok || total != 100000) { fprintf(stderr, "tcpstress: got %zu bytes%s\n", total, ok ? "" : ", corrupt"); return 1; }
    if (send(c, "thanks", 6, 0) != 6) { perror("tcpstress: send after FIN"); return 1; }
    close(c);
    int st;
    waitpid(child, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { fprintf(stderr, "tcpstress: client status %x\n", st); return 1; }
    close(l);
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in na = { .sin_family = AF_INET, .sin_port = htons(8089), .sin_addr.s_addr = htonl(0x7F000001) };
    int rc = connect(probe, (struct sockaddr *)&na, sizeof na);
    if (rc == 0 || errno != ECONNREFUSED) { fprintf(stderr, "tcpstress: closed port not refused (%s)\n", rc == 0 ? "connected" : strerror(errno)); return 1; }
    close(probe);
    return 0;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200;
    for (int i = 1; i <= n; i++) {
        if (round_() != 0) { printf("tcpstress: round %d failed\n", i); return 1; }
        if (i % 50 == 0) printf("tcpstress: %d rounds\n", i);
    }
    printf("tcpstress: %d rounds ok\n", n);
    return 0;
}
