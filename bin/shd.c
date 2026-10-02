/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* shd [PORT]: a shell for whoever connects (default port 2323), for
 * driving a real machine from another one over the LAN: `nc HOST 2323`.
 * No password and no encryption: run it only on a network you trust, and
 * stop it (^C) when done. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 2323;
    int l = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 4) != 0) { perror("shd"); return 1; }
    printf("shd: a shell on port %d for anyone who connects; ^C stops it\n", port);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int c = accept(l, (struct sockaddr *)&peer, &plen);
        if (c < 0) { perror("shd: accept"); continue; }
        printf("shd: %s connected\n", inet_ntoa(peer.sin_addr));
        pid_t pid = fork();
        if (pid == 0) {
            close(l);
            dup2(c, 0); dup2(c, 1); dup2(c, 2);
            if (c > 2) close(c);
            execl("/bin/sh", "sh", (char *)NULL);
            _exit(127);
        }
        close(c);
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
    }
}
