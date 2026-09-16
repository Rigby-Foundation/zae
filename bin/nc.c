/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* nc: connect or listen, then shuttle bytes between the socket and the terminal.
 *
 *   nc [-u] HOST PORT        connect (TCP, or UDP with -u)
 *   nc [-u] -l PORT          listen; TCP accepts one connection
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

int main(int argc, char **argv)
{
    int udp = 0, listen_mode = 0;
    const char *host = NULL, *port = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) udp = 1;
        else if (strcmp(argv[i], "-l") == 0) listen_mode = 1;
        else if (!host && !listen_mode) host = argv[i];
        else port = argv[i];
    }
    if (!port) { fprintf(stderr, "usage: nc [-u] HOST PORT | nc [-u] -l PORT\n"); return 2; }

    int type = udp ? SOCK_DGRAM : SOCK_STREAM;
    int s = socket(AF_INET, type, 0);
    if (s < 0) { perror("nc: socket"); return 1; }
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof peer;
    int have_peer = 0;

    if (listen_mode) {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(port)), .sin_addr.s_addr = INADDR_ANY };
        if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("nc: bind"); return 1; }
        if (!udp) {
            if (listen(s, 1) < 0) { perror("nc: listen"); return 1; }
            int c = accept(s, (struct sockaddr *)&peer, &peerlen);
            if (c < 0) { perror("nc: accept"); return 1; }
            char buf[32];
            fprintf(stderr, "nc: connection from %s:%u\n", inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof buf), ntohs(peer.sin_port));
            close(s);
            s = c;
            have_peer = 1;
        }
    } else {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = type }, *res;
        int gai = getaddrinfo(host, port, &hints, &res);
        if (gai) { fprintf(stderr, "nc: %s: %s\n", host, gai_strerror(gai)); return 1; }
        peer = *(struct sockaddr_in *)res->ai_addr;
        freeaddrinfo(res);
        if (connect(s, (struct sockaddr *)&peer, sizeof peer) < 0) { perror("nc: connect"); return 1; }
        have_peer = 1;
    }

    struct pollfd fds[2] = { { 0, POLLIN, 0 }, { s, POLLIN, 0 } };
    int stdin_open = 1;
    for (;;) {
        fds[0].fd = stdin_open ? 0 : -1;
        if (poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; perror("nc: poll"); return 1; }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char buf[1024];
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) {
                stdin_open = 0;
                if (!udp) shutdown(s, SHUT_WR);
            } else if (have_peer) {
                if (send(s, buf, (size_t)n, 0) < 0) { perror("nc: send"); return 1; }
            }
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[2048];
            ssize_t n = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&peer, &peerlen);
            if (n < 0) { perror("nc: recv"); return 1; }
            if (n == 0) break;
            have_peer = 1;
            write(1, buf, (size_t)n);
        }
    }
    close(s);
    return 0;
}
