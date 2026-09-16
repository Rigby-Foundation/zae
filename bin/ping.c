/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* ping: ICMP echo over a raw socket.   ping [-c COUNT] HOST */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <netdb.h>

static volatile int stop;
static void on_int(int sig) { (void)sig; stop = 1; }

static uint16_t csum(const void *data, size_t len)
{
    const uint8_t *b = data;
    uint32_t sum = 0;
    for (; len > 1; len -= 2, b += 2) sum += (uint32_t)(b[0] << 8 | b[1]);
    if (len) sum += (uint32_t)(b[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    int count = 4;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) count = atoi(argv[++i]);
        else host = argv[i];
    }
    if (!host) { fprintf(stderr, "usage: ping [-c COUNT] HOST\n"); return 2; }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_RAW }, *res;
    int gai = getaddrinfo(host, NULL, &hints, &res);
    if (gai) { fprintf(stderr, "ping: %s: %s\n", host, gai_strerror(gai)); return 1; }
    struct sockaddr_in dst = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    char addr[32];
    inet_ntop(AF_INET, &dst.sin_addr, addr, sizeof addr);

    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) { perror("ping: socket"); return 1; }
    signal(SIGINT, on_int);
    uint16_t id = (uint16_t)getpid();
    int sent = 0, got = 0;
    double min = 1e9, max = 0, total = 0;
    printf("PING %s (%s): 56 data bytes\n", host, addr);
    for (int seq = 0; (count <= 0 || seq < count) && !stop; seq++) {
        uint8_t pkt[64];
        struct icmphdr *ic = (struct icmphdr *)pkt;
        memset(pkt, 0, sizeof pkt);
        ic->type = ICMP_ECHO;
        ic->un.echo.id = htons(id);
        ic->un.echo.sequence = htons((uint16_t)seq);
        for (int i = 8; i < 64; i++) pkt[i] = (uint8_t)i;
        ic->checksum = csum(pkt, sizeof pkt);
        double t0 = now_ms();
        if (sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&dst, sizeof dst) < 0) {
            perror("ping: sendto");
            sleep(1);
            continue;
        }
        sent++;
        int replied = 0;
        for (;;) {
            long left = (long)(1000 - (now_ms() - t0));
            if (left <= 0) break;
            struct pollfd p = { s, POLLIN, 0 };
            if (poll(&p, 1, (int)left) <= 0) break;
            uint8_t buf[1500];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n < 28) continue;
            struct ip *ip = (struct ip *)buf;
            int hl = ip->ip_hl * 4;
            struct icmphdr *r = (struct icmphdr *)(buf + hl);
            if (r->type != ICMP_ECHOREPLY || ntohs(r->un.echo.id) != id || ntohs(r->un.echo.sequence) != seq) continue;
            double rtt = now_ms() - t0;
            got++;
            replied = 1;
            if (rtt < min) min = rtt;
            if (rtt > max) max = rtt;
            total += rtt;
            printf("%ld bytes from %s: icmp_seq=%d ttl=%d time=%.2f ms\n", (long)(n - hl), inet_ntop(AF_INET, &from.sin_addr, addr, sizeof addr), seq, ip->ip_ttl, rtt);
            break;
        }
        if (!replied) printf("request timeout for icmp_seq %d\n", seq);
        if ((count <= 0 || seq + 1 < count) && !stop) {
            long left = (long)(1000 - (now_ms() - t0));
            if (left > 0) usleep((useconds_t)left * 1000);
        }
    }
    printf("--- %s ping statistics ---\n%d packets transmitted, %d received, %d%% packet loss\n",
           host, sent, got, sent ? (sent - got) * 100 / sent : 0);
    if (got) printf("round-trip min/avg/max = %.2f/%.2f/%.2f ms\n", min, total / got, max);
    return got ? 0 : 1;
}
