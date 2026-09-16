/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* httpd: a small static file server.   httpd [-p PORT] [DIR]   (default 80, /)
 * Serves GET requests one at a time; directories get a generated index. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *root = "/";

static void send_all(int c, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = send(c, p, len, MSG_NOSIGNAL);
        if (n <= 0) return;
        p += n;
        len -= (size_t)n;
    }
}

static void respond(int c, int status, const char *reason, const char *type, const char *body, size_t len)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr, "HTTP/1.0 %d %s\r\nServer: sic-httpd/1.0\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, reason, type, len);
    send_all(c, hdr, (size_t)n);
    if (body) send_all(c, body, len);
}

static const char *mime(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcmp(ext, ".html") || !strcmp(ext, ".htm")) return "text/html";
    if (!strcmp(ext, ".txt") || !strcmp(ext, ".c") || !strcmp(ext, ".h") || !strcmp(ext, ".md")) return "text/plain";
    if (!strcmp(ext, ".css")) return "text/css";
    if (!strcmp(ext, ".js")) return "application/javascript";
    if (!strcmp(ext, ".png")) return "image/png";
    if (!strcmp(ext, ".jpg")) return "image/jpeg";
    return "application/octet-stream";
}

static void serve(int c)
{
    char req[2048];
    size_t have = 0;
    for (;;) {
        ssize_t n = recv(c, req + have, sizeof req - 1 - have, 0);
        if (n <= 0) return;
        have += (size_t)n;
        req[have] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n") || have == sizeof req - 1) break;
    }
    char method[8], target[512];
    if (sscanf(req, "%7s %511s", method, target) != 2) { respond(c, 400, "Bad Request", "text/plain", "bad request\n", 12); return; }
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) { respond(c, 405, "Method Not Allowed", "text/plain", "GET only\n", 9); return; }
    char *q = strchr(target, '?');
    if (q) *q = 0;
    if (strstr(target, "..")) { respond(c, 403, "Forbidden", "text/plain", "forbidden\n", 10); return; }
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", root, root[strlen(root) - 1] == '/' ? "" : "/", target + (target[0] == '/'));
    struct stat st;
    if (stat(path, &st) != 0) { respond(c, 404, "Not Found", "text/plain", "not found\n", 10); return; }

    if (S_ISDIR(st.st_mode)) {
        char idx[1100];
        snprintf(idx, sizeof idx, "%s/index.html", path);
        if (stat(idx, &st) == 0 && S_ISREG(st.st_mode)) {
            strcpy(path, idx);
        } else {
            char *body = malloc(65536);
            if (!body) return;
            int n = snprintf(body, 65536, "<html><head><title>%s</title></head><body><h1>Index of %s</h1><ul>\n", target, target);
            DIR *d = opendir(path);
            struct dirent *e;
            while (d && (e = readdir(d)) && n < 60000) {
                if (e->d_name[0] == '.') continue;
                n += snprintf(body + n, 65536 - (size_t)n, "<li><a href=\"%s%s%s\">%s</a></li>\n",
                              target, target[strlen(target) - 1] == '/' ? "" : "/", e->d_name, e->d_name);
            }
            if (d) closedir(d);
            n += snprintf(body + n, 65536 - (size_t)n, "</ul><hr><i>sic httpd</i></body></html>\n");
            respond(c, 200, "OK", "text/html", body, (size_t)n);
            free(body);
            return;
        }
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { respond(c, 403, "Forbidden", "text/plain", "cannot open\n", 12); return; }
    respond(c, 200, "OK", mime(path), NULL, (size_t)st.st_size);
    if (strcmp(method, "GET") == 0) {
        char buf[8192];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0)
            send_all(c, buf, (size_t)n);
    }
    close(fd);
}

int main(int argc, char **argv)
{
    int port = 80;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else root = argv[i];
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("httpd: socket"); return 1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("httpd: bind"); return 1; }
    if (listen(s, 8) < 0) { perror("httpd: listen"); return 1; }
    printf("httpd: serving %s on port %d\n", root, port);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int c = accept(s, (struct sockaddr *)&peer, &pl);
        if (c < 0) { if (errno == EINTR) continue; perror("httpd: accept"); return 1; }
        serve(c);
        close(c);
    }
}
