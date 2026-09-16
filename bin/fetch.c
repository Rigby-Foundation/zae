/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* fetch: HTTP/1.0 GET.   fetch [-o FILE] [-v] http://HOST[:PORT]/PATH */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

int main(int argc, char **argv)
{
    const char *url = NULL, *out = NULL;
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else url = argv[i];
    }
    if (!url) { fprintf(stderr, "usage: fetch [-o FILE] [-v] http://HOST[:PORT]/PATH\n"); return 2; }
    if (strncmp(url, "http://", 7) == 0) url += 7;
    char host[256], port[8] = "80";
    const char *path = strchr(url, '/');
    size_t hl = path ? (size_t)(path - url) : strlen(url);
    if (hl >= sizeof host) { fprintf(stderr, "fetch: host too long\n"); return 2; }
    memcpy(host, url, hl);
    host[hl] = 0;
    if (!path) path = "/";
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; snprintf(port, sizeof port, "%s", colon + 1); }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai) { fprintf(stderr, "fetch: %s: %s\n", host, gai_strerror(gai)); return 1; }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("fetch: socket"); return 1; }
    if (verbose) {
        char a[32];
        fprintf(stderr, "* connecting to %s:%s\n", inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, a, sizeof a), port);
    }
    if (connect(s, res->ai_addr, res->ai_addrlen) < 0) { perror("fetch: connect"); return 1; }
    freeaddrinfo(res);

    char req[1024];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: sic-fetch/1.0\r\nConnection: close\r\n\r\n", path, host);
    if (send(s, req, (size_t)n, 0) != n) { perror("fetch: send"); return 1; }

    int fd = 1;
    if (out) {
        fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror(out); return 1; }
    }
    /* headers: print with -v, always strip them from the body */
    char buf[4096];
    size_t have = 0;
    int in_body = 0, status = 0;
    long total = 0;
    for (;;) {
        ssize_t r = recv(s, buf + have, sizeof buf - have, 0);
        if (r < 0) { perror("fetch: recv"); return 1; }
        if (r == 0) break;
        have += (size_t)r;
        if (!in_body) {
            buf[have < sizeof buf ? have : sizeof buf - 1] = 0;
            char *end = strstr(buf, "\r\n\r\n");
            if (!end && have < sizeof buf) continue;
            size_t hdr = end ? (size_t)(end + 4 - buf) : have;
            if (sscanf(buf, "HTTP/%*d.%*d %d", &status) != 1) status = 0;
            if (verbose) fwrite(buf, 1, hdr, stderr);
            memmove(buf, buf + hdr, have - hdr);
            have -= hdr;
            in_body = 1;
        }
        if (have) {
            if (write(fd, buf, have) != (ssize_t)have) { perror("fetch: write"); return 1; }
            total += (long)have;
            have = 0;
        }
    }
    if (out) {
        close(fd);
        fprintf(stderr, "fetch: %ld bytes -> %s (HTTP %d)\n", total, out, status);
    }
    close(s);
    return status >= 200 && status < 400 ? 0 : 1;
}
