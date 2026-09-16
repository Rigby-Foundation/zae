/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* dhcp: a one-shot DHCPv4 client. Configures the interface (address, mask,
 * gateway) and writes /etc/resolv.conf from the lease. No renewal: leases
 * on the networks this runs on are long enough, and re-running it is cheap.
 *
 *   dhcp [-q] [-t SECONDS] [IF]     (default: eth0, 10 s)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SIOCSIFGATEWAY 0x89F1

struct dhcp_msg {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64], file[128];
    uint32_t magic;
    uint8_t  options[312];
} __attribute__((packed));

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

static int quiet;
static unsigned char mac[6];
static uint32_t xid;

static int build(struct dhcp_msg *m, int type, uint32_t server, uint32_t requested)
{
    memset(m, 0, sizeof(*m));
    m->op = 1; m->htype = 1; m->hlen = 6;
    m->xid = xid;
    m->flags = htons(0x8000);               /* please broadcast the reply */
    memcpy(m->chaddr, mac, 6);
    m->magic = htonl(0x63825363);
    uint8_t *o = m->options;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    if (requested) { *o++ = 50; *o++ = 4; memcpy(o, &requested, 4); o += 4; }
    if (server)    { *o++ = 54; *o++ = 4; memcpy(o, &server, 4); o += 4; }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15;   /* mask, router, dns, domain */
    *o++ = 12; *o++ = 3; memcpy(o, "sic", 3); o += 3;
    *o++ = 255;
    return (int)(o - (uint8_t *)m);
}

static const uint8_t *option(const struct dhcp_msg *m, int code, int *len)
{
    const uint8_t *o = m->options, *end = m->options + sizeof(m->options);
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 1 >= end) break;
        if (*o == code) { *len = o[1]; return o + 2; }
        o += 2 + o[1];
    }
    return NULL;
}

/* Wait up to `ms` for a reply of the wanted type to our xid. */
static int wait_reply(int s, struct dhcp_msg *m, int want, int ms)
{
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left = ms - ((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) return -1;
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, (int)left) <= 0) return -1;
        ssize_t n = recv(s, m, sizeof(*m), 0);
        if (n < (ssize_t)(sizeof(*m) - sizeof(m->options)) || m->op != 2 || m->xid != xid) continue;
        int len;
        const uint8_t *t = option(m, 53, &len);
        if (t && len == 1 && *t == want) return 0;
        if (t && len == 1 && *t == DHCPNAK) return -2;
    }
}

int main(int argc, char **argv)
{
    const char *ifname = "eth0";
    int timeout = 10;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) quiet = 1;
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) timeout = atoi(argv[++i]);
        else ifname = argv[i];
    }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("dhcp: socket"); return 1; }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) { fprintf(stderr, "dhcp: %s: %s\n", ifname, strerror(errno)); return 1; }
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(68), .sin_addr.s_addr = INADDR_ANY };
    if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) { perror("dhcp: bind"); return 1; }
    struct sockaddr_in bcast = { .sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = INADDR_BROADCAST };

    srand((unsigned)(time(NULL) ^ (mac[5] << 8 | mac[4])));
    xid = (uint32_t)rand() ^ ((uint32_t)mac[5] << 24);

    struct dhcp_msg m;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t offered = 0, server = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec >= timeout) { if (!quiet) fprintf(stderr, "dhcp: no offer on %s\n", ifname); return 1; }
        int n = build(&m, DHCPDISCOVER, 0, 0);
        if (sendto(s, &m, n, 0, (struct sockaddr *)&bcast, sizeof(bcast)) < 0) { perror("dhcp: send"); return 1; }
        if (wait_reply(s, &m, DHCPOFFER, 2000) == 0) {
            int len;
            const uint8_t *sid = option(&m, 54, &len);
            offered = m.yiaddr;
            if (sid && len == 4) memcpy(&server, sid, 4); else server = m.siaddr;
            break;
        }
    }
    for (int tries = 0; ; tries++) {
        if (tries == 4) { if (!quiet) fprintf(stderr, "dhcp: no ack from server\n"); return 1; }
        int n = build(&m, DHCPREQUEST, server, offered);
        sendto(s, &m, n, 0, (struct sockaddr *)&bcast, sizeof(bcast));
        int r = wait_reply(s, &m, DHCPACK, 2000);
        if (r == 0) break;
        if (r == -2) { if (!quiet) fprintf(stderr, "dhcp: request refused (NAK)\n"); return 1; }
    }

    /* apply the lease */
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    int len;
    const uint8_t *o;
    memset(&ifr.ifr_addr, 0, sizeof(ifr.ifr_addr));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = m.yiaddr;
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) { perror("dhcp: SIOCSIFADDR"); return 1; }
    uint32_t mask = htonl(0xFFFFFF00), gw = 0;
    if ((o = option(&m, 1, &len)) && len == 4) memcpy(&mask, o, 4);
    sin->sin_addr.s_addr = mask;
    ioctl(s, SIOCSIFNETMASK, &ifr);
    if ((o = option(&m, 3, &len)) && len >= 4) {
        memcpy(&gw, o, 4);
        sin->sin_addr.s_addr = gw;
        ioctl(s, SIOCSIFGATEWAY, &ifr);
    }
    if ((o = option(&m, 6, &len)) && len >= 4) {
        FILE *f = fopen("/etc/resolv.conf", "w");
        if (f) {
            for (int i = 0; i + 4 <= len; i += 4) {
                char buf[32];
                fprintf(f, "nameserver %s\n", inet_ntop(AF_INET, o + i, buf, sizeof buf));
            }
            fclose(f);
        }
    }
    if (!quiet) {
        char a[32], mk[32], g[32];
        printf("dhcp: %s: %s netmask %s", ifname, inet_ntop(AF_INET, &m.yiaddr, a, sizeof a), inet_ntop(AF_INET, &mask, mk, sizeof mk));
        if (gw) printf(" gateway %s", inet_ntop(AF_INET, &gw, g, sizeof g));
        if ((o = option(&m, 6, &len)) && len >= 4) printf(" dns %s", inet_ntop(AF_INET, o, a, sizeof a));
        printf("\n");
    }
    return 0;
}
