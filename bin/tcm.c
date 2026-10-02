/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* tcm: talk to a Synaptics/OmniVision TouchComm controller on /dev/spi0.
 * A command is [code, length LE16, payload]; the device answers with
 * [0xa5, status or report code, length LE16], then, read again,
 * [0xa5, 0x03, payload, 0x5a].
 *   tcm id        identify: protocol, mode, part number
 *   tcm boot B    ROM bootloader: download the image's ROMBOOT_APP_CODE
 *                 (command 0x45: [B, 13 zero bytes, the code]), then identify
 *   tcm run       start what was downloaded (0x42), then identify
 *   tcm config V  host-download firmware: send the touch config (0x30:
 *                 [V, 1 (touch config), APP_CONFIG, padding to 8 bytes]);
 *                 V is the status report's HDL version + 1
 *   tcm watch S   print every message for S seconds (busy ones counted)
 *   tcm repcfg    the touch report config (0x25): how reports are laid out
 *   tcm info      application info (0x20): the coordinate range, objects
 *   tcm cmd C B.. any command C with payload bytes B.. (hex), and its answer
 * Options before the command: -c BYTES (write piece size), -d US (pause
 * between pieces), -r (pieces after the first without the 0x01 marker),
 * -s (the whole message in one transfer).
 * Writes longer than the device takes at once (its max write size) go in
 * pieces: the first [code, length LE16, data], the rest [0x01, data]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define MARKER      0xa5
#define CONTINUED   0x03

#define IMAGE       "/lib/firmware/omnivision_hdl_firmware.img"

static int spi;
static unsigned int max_write = 1024;
static unsigned int chunk_delay_us;             /* between the pieces of a long write */
static int raw_continue;                        /* pieces after the first carry no 0x01 (the ROM bootloader counts it as data) */
static int single;                              /* the whole message in one transfer (chip select held throughout) */
static unsigned char in[4096];
static unsigned int in_len;

static int send_cmd(unsigned char code, const unsigned char *payload, unsigned int len)
{
    static unsigned char buf[4096];
    if (single) {
        unsigned char *all = malloc(len + 3);
        all[0] = code; all[1] = (unsigned char)len; all[2] = (unsigned char)(len >> 8);
        if (len) memcpy(all + 3, payload, len);
        int ok = write(spi, all, len + 3) == (ssize_t)(len + 3);
        free(all);
        return ok ? 0 : -1;
    }
    unsigned int first = len < max_write - 3 ? len : max_write - 3, done;
    buf[0] = code; buf[1] = (unsigned char)len; buf[2] = (unsigned char)(len >> 8);
    if (first) memcpy(buf + 3, payload, first);
    if (write(spi, buf, 3 + first) != (ssize_t)(3 + first)) return -1;
    for (done = first; done < len; ) {
        unsigned int hdr = raw_continue ? 0 : 1;
        unsigned int n = len - done < max_write - hdr ? len - done : max_write - hdr;
        buf[0] = 0x01;                          /* CMD_CONTINUE_WRITE */
        memcpy(buf + hdr, payload + done, n);
        if (chunk_delay_us) usleep(chunk_delay_us);
        if (write(spi, buf, hdr + n) != (ssize_t)(hdr + n)) return -1;
        done += n;
    }
    return 0;
}

/* An area of the firmware image by name: its data and length. */
static unsigned char *image;
static long image_len;

static const unsigned char *area(const char *name, unsigned int *len)
{
    if (!image) {
        FILE *f = fopen(IMAGE, "rb");
        if (!f) { perror(IMAGE); return NULL; }
        image = malloc(1 << 20);
        image_len = (long)fread(image, 1, 1 << 20, f);
        fclose(f);
    }
    unsigned int n = image[4] | image[5] << 8;
    for (unsigned int i = 0; i < n; i++) {
        unsigned int off = image[8 + 4 * i] | image[9 + 4 * i] << 8 | image[10 + 4 * i] << 16 | (unsigned)image[11 + 4 * i] << 24;
        if ((long)off + 0x24 > image_len) continue;
        if (strncmp((const char *)image + off + 4, name, strlen(name)) != 0) continue;
        *len = image[off + 0x1c] | image[off + 0x1d] << 8 | image[off + 0x1e] << 16 | (unsigned)image[off + 0x1f] << 24;
        return image + off + 0x24;
    }
    fprintf(stderr, "tcm: no %s in the image\n", name);
    return NULL;
}

/* The next message: its code (-1: nothing), payload in `in`. */
static int recv_msg(void)
{
    unsigned char h[4];
    if (read(spi, h, 4) != 4 || h[0] != MARKER) return -1;
    in_len = (unsigned int)h[2] | (unsigned int)h[3] << 8;
    if (!in_len) return h[1];
    static unsigned char buf[4096 + 3];
    if (in_len + 3 > sizeof buf || read(spi, buf, in_len + 3) != (ssize_t)(in_len + 3)) return -1;
    if (buf[0] != MARKER || buf[1] != CONTINUED) { fprintf(stderr, "tcm: continued read gave %02x %02x\n", buf[0], buf[1]); return -1; }
    memcpy(in, buf + 2, in_len);
    return h[1];
}

static void show(int code)
{
    printf("code %02x, %u bytes:", code, in_len);
    for (unsigned int i = 0; i < in_len && i < 256; i++) printf(" %02x", in[i]);
    printf("%s\n", in_len > 256 ? " ..." : "");
}

/* Every message for up to `ms` (busy ones skipped); returns the last
 * status code (< 0x10), -1 if none. */
static int drain(int ms)
{
    int status = -1;
    for (int t = 0; t < ms; t += 5) {
        int code = recv_msg();
        if (code <= 0) { usleep(5000); continue; }      /* nothing, or idle */
        if (code == 0x02) { usleep(5000); continue; }    /* busy: the answer is still coming */
        show(code);
        if (code == 0x10 && in_len >= 18) {
            char part[17] = "";
            memcpy(part, in + 2, 16);
            printf("  identify: protocol %u, mode %02x, part \"%s\", max write %u\n", in[0], in[1], part,
                   in_len >= 24 ? (unsigned)(in[22] | in[23] << 8) : 0);
        }
        if (code < 0x10) status = code;
    }
    return status;
}

int main(int argc, char **argv)
{
    while (argc > 3 && argv[1][0] == '-') {
        if (argv[1][1] == 'c') max_write = (unsigned)atoi(argv[2]);
        else if (argv[1][1] == 'd') chunk_delay_us = (unsigned)atoi(argv[2]);
        else if (argv[1][1] == 'r') { raw_continue = 1; argv += 1; argc -= 1; continue; }
        else if (argv[1][1] == 's') { single = 1; argv += 1; argc -= 1; continue; }
        argv += 2; argc -= 2;
    }
    if (argc < 2) { fprintf(stderr, "usage: tcm [-c chunk] [-d us] id | boot B | run\n"); return 2; }
    spi = open("/dev/spi0", O_RDWR);
    if (spi < 0) { perror("/dev/spi0"); return 1; }
    if (!strcmp(argv[1], "id")) {
        drain(20);                              /* whatever was waiting */
        send_cmd(0x02, NULL, 0);
        int st = drain(200);
        printf("status %02x\n", st & 0xff);
        if (st == 0x01 && in_len >= 18) {       /* the response to identify carries the same fields */
            char part[17] = "";
            memcpy(part, in + 2, 16);
            printf("  protocol %u, mode %02x, part \"%s\", max write %u\n", in[0], in[1], part,
                   in_len >= 24 ? (unsigned)(in[22] | in[23] << 8) : 0);
        }
        return 0;
    }
    if (!strcmp(argv[1], "bootrun") && argc > 2) {        /* download, then start it at once */
        unsigned int len;
        const unsigned char *code = area("ROMBOOT_APP_CODE", &len);
        if (!code) return 1;
        unsigned char *msg = calloc(1, len + 14);
        msg[0] = (unsigned char)strtoul(argv[2], NULL, 16);
        memcpy(msg + 14, code, len);
        drain(20);
        if (send_cmd(0x45, msg, len + 14) != 0) { perror("write"); return 1; }
        int st = -1;
        for (int t = 0; t < 15000 && st < 0; t += 100) st = drain(100);
        printf("downloaded; status %02x\n", st & 0xff);
        send_cmd(0x42, NULL, 0);
        printf("run; status %02x\n", drain(1000) & 0xff);
        send_cmd(0x02, NULL, 0);
        drain(300);
        return 0;
    }
    if (!strcmp(argv[1], "boot") && argc > 2) {
        unsigned int len;
        const unsigned char *code = area("ROMBOOT_APP_CODE", &len);
        if (!code) return 1;
        unsigned char *msg = calloc(1, len + 14);
        msg[0] = (unsigned char)strtoul(argv[2], NULL, 16);
        memcpy(msg + 14, code, len);
        drain(20);
        printf("romboot download: %u bytes, first byte %02x\n", len, msg[0]);
        if (send_cmd(0x45, msg, len + 14) != 0) { perror("write"); return 1; }
        int st = drain(5000);
        printf("status %02x\n", st & 0xff);
        send_cmd(0x02, NULL, 0);
        drain(200);
        return 0;
    }
    if (!strcmp(argv[1], "config") && argc > 2) {
        unsigned int len;
        const unsigned char *cfg = area("APP_CONFIG", &len);
        if (!cfg) return 1;
        unsigned int pad = (8 - len % 8) % 8;
        unsigned char *msg = calloc(1, len + 2 + pad);
        msg[0] = (unsigned char)strtoul(argv[2], NULL, 16);
        msg[1] = 1;                                     /* HDL_TOUCH_CONFIG */
        memcpy(msg + 2, cfg, len);
        drain(20);
        printf("config download: %u bytes (+%u padding), version byte %02x\n", len, pad, msg[0]);
        if (send_cmd(0x30, msg, len + 2 + pad) != 0) { perror("write"); return 1; }
        int st = -1;
        for (int t = 0; t < 15000 && st < 0; t += 100) st = drain(100);
        printf("status %02x\n", st & 0xff);
        drain(2000);
        send_cmd(0x02, NULL, 0);
        drain(200);
        return 0;
    }
    if (!strcmp(argv[1], "cmd") && argc > 2) {
        unsigned char payload[256];
        unsigned int n = 0;
        for (int i = 3; i < argc && n < sizeof payload; i++) payload[n++] = (unsigned char)strtoul(argv[i], NULL, 16);
        drain(20);
        send_cmd((unsigned char)strtoul(argv[2], NULL, 16), payload, n);
        printf("status %02x\n", drain(300) & 0xff);
        return 0;
    }
    if (!strcmp(argv[1], "info")) {
        drain(20);
        send_cmd(0x20, NULL, 0);
        int st = drain(300);
        if (st == 0x01 && in_len >= 38)
            printf("max x %u, max y %u, max objects %u\n", in[32] | in[33] << 8, in[34] | in[35] << 8, in[36] | in[37] << 8);
        return 0;
    }
    if (!strcmp(argv[1], "repcfg")) {
        drain(20);
        send_cmd(0x25, NULL, 0);
        int st = drain(300);
        printf("status %02x\n", st & 0xff);
        if (st == 0x01) {
            for (unsigned int i = 0; i < in_len; i++) printf("%02x%s", in[i], i % 16 == 15 || i == in_len - 1 ? "\n" : " ");
        }
        return 0;
    }
    if (!strcmp(argv[1], "watch") && argc > 2) {
        int secs = atoi(argv[2]), busy = 0, idle = 0;
        for (int t = 0; t < secs * 1000; t += 2) {
            int code = recv_msg();
            if (code < 0 || code == 0) { idle++; usleep(2000); continue; }
            if (code == 0x02) { busy++; usleep(2000); continue; }
            show(code);
        }
        printf("(%d busy, %d idle polls)\n", busy, idle);
        return 0;
    }
    if (!strcmp(argv[1], "run")) {
        drain(20);
        send_cmd(0x42, NULL, 0);
        int st = drain(300);
        printf("status %02x\n", st & 0xff);
        send_cmd(0x02, NULL, 0);
        drain(200);
        return 0;
    }
    fprintf(stderr, "tcm: unknown command %s\n", argv[1]);
    return 2;
}
