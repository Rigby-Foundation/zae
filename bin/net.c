/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* net: show and configure network interfaces.
 *
 *   net                          list interfaces
 *   net IF ADDR MASK [GATEWAY]   configure IF (and bring it up)
 *   net IF up|down
 *   net dns SERVER...            write /etc/resolv.conf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SIOCGIFGATEWAY 0x89F0       /* sic extension: per-interface default gateway */
#define SIOCSIFGATEWAY 0x89F1

static int sock;

static int get_addr(const char *name, unsigned long req, struct in_addr *out)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(sock, req, &ifr) < 0) return -1;
    *out = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    return 0;
}

static int set_addr(const char *name, unsigned long req, const char *text)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, text, &sin->sin_addr) != 1) {
        fprintf(stderr, "net: bad address '%s'\n", text);
        return -1;
    }
    if (ioctl(sock, req, &ifr) < 0) {
        perror("net: ioctl");
        return -1;
    }
    return 0;
}

static void show(const char *name)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) return;
    int flags = ifr.ifr_flags;
    printf("%s: %s%s%s", name, (flags & IFF_UP) ? "up" : "down",
           (flags & IFF_LOOPBACK) ? " loopback" : "", (flags & IFF_RUNNING) ? " link" : "");
    if (ioctl(sock, SIOCGIFMTU, &ifr) == 0) printf(" mtu %d", ifr.ifr_mtu);
    if (!(flags & IFF_LOOPBACK) && ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
        unsigned char *m = (unsigned char *)ifr.ifr_hwaddr.sa_data;
        printf(" mac %02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    printf("\n");
    struct in_addr a, m, g;
    char sa[32], sm[32], sg[32];
    if (get_addr(name, SIOCGIFADDR, &a) == 0 && a.s_addr) {
        get_addr(name, SIOCGIFNETMASK, &m);
        get_addr(name, SIOCGIFGATEWAY, &g);
        printf("    inet %s netmask %s", inet_ntop(AF_INET, &a, sa, sizeof sa), inet_ntop(AF_INET, &m, sm, sizeof sm));
        if (g.s_addr) printf(" gateway %s", inet_ntop(AF_INET, &g, sg, sizeof sg));
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("net: socket"); return 1; }

    if (argc == 1) {
        for (int i = 1; i < 16; i++) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            ifr.ifr_ifindex = i;
            if (ioctl(sock, SIOCGIFNAME, &ifr) < 0) continue;
            show(ifr.ifr_name);
        }
        return 0;
    }
    if (strcmp(argv[1], "dns") == 0) {
        FILE *f = fopen("/etc/resolv.conf", "w");
        if (!f) { perror("net: /etc/resolv.conf"); return 1; }
        for (int i = 2; i < argc; i++) fprintf(f, "nameserver %s\n", argv[i]);
        fclose(f);
        return 0;
    }
    if (argc == 3 && (strcmp(argv[2], "up") == 0 || strcmp(argv[2], "down") == 0)) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, argv[1], IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) { perror("net"); return 1; }
        if (strcmp(argv[2], "up") == 0) ifr.ifr_flags |= IFF_UP; else ifr.ifr_flags &= ~IFF_UP;
        if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) { perror("net"); return 1; }
        return 0;
    }
    if (argc == 2) {
        show(argv[1]);
        return 0;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: net | net IF | net IF ADDR MASK [GATEWAY] | net IF up|down | net dns SERVER...\n");
        return 2;
    }
    if (set_addr(argv[1], SIOCSIFADDR, argv[2]) || set_addr(argv[1], SIOCSIFNETMASK, argv[3]))
        return 1;
    if (argc > 4 && set_addr(argv[1], SIOCSIFGATEWAY, argv[4]))
        return 1;
    show(argv[1]);
    return 0;
}
