/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* mkfs.fat: format a block device with FAT32 (or FAT16 with -F 16). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

#define SS 512

static int fd;
/* FAT is little-endian on disk whatever the CPU. */
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static int put(uint64_t sector, const void *buf, size_t n)
{
    if (lseek(fd, (off_t)(sector * SS), SEEK_SET) < 0) return -1;
    return write(fd, buf, n) == (ssize_t)n ? 0 : -1;
}

int main(int argc, char **argv)
{
    int bits = 32;
    const char *dev = NULL, *label = "NO NAME";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) bits = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) label = argv[++i];
        else dev = argv[i];
    }
    if (!dev || (bits != 16 && bits != 32)) { fprintf(stderr, "usage: mkfs.fat [-F 16|32] [-n label] <device>\n"); return 2; }
    fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0) { fprintf(stderr, "%s: cannot determine size\n", dev); return 1; }
    uint32_t total = (uint32_t)(size / SS);

    uint8_t bs[SS];
    memset(bs, 0, SS);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "SICFAT  ", 8);
    uint16_t bps = SS;
    uint8_t spc = bits == 32 ? (total < 0x100000 ? 1 : 8) : (total < 0x10000 ? 4 : 32);
    uint16_t reserved = bits == 32 ? 32 : 4;
    uint8_t fats = 2;
    uint16_t root_entries = bits == 32 ? 0 : 512;
    uint32_t root_sectors = (root_entries * 32 + SS - 1) / SS;
    /* FAT size: iterate until it fits. */
    uint32_t fat_sectors = 1;
    for (int it = 0; it < 4; it++) {
        uint32_t data = total - reserved - fats * fat_sectors - root_sectors;
        uint32_t clusters = data / spc + 2;
        uint32_t bytes = bits == 32 ? clusters * 4 : clusters * 2;
        fat_sectors = (bytes + SS - 1) / SS;
    }
    uint32_t clusters = (total - reserved - fats * fat_sectors - root_sectors) / spc;
    if ((bits == 32 && clusters < 65525) || (bits == 16 && (clusters < 4085 || clusters >= 65525))) {
        /* Steer the cluster count into the right range for the requested type. */
        if (bits == 32) { fprintf(stderr, "mkfs.fat: device too small for FAT32 with %u-sector clusters; using FAT16\n", spc); bits = 16; root_entries = 512; root_sectors = 32; reserved = 4;
            spc = 4; for (int it = 0; it < 4; it++) { uint32_t data = total - reserved - fats * fat_sectors - root_sectors; fat_sectors = ((data / spc + 2) * 2 + SS - 1) / SS; }
            clusters = (total - reserved - fats * fat_sectors - root_sectors) / spc; }
        if (bits == 16 && clusters < 4085) { fprintf(stderr, "mkfs.fat: device too small for FAT16\n"); return 1; }
    }

    w16(bs + 11, bps);
    bs[13] = spc;
    w16(bs + 14, reserved);
    bs[16] = fats;
    w16(bs + 17, root_entries);
    if (total < 0x10000) w16(bs + 19, (uint16_t)total); else w32(bs + 32, total);
    bs[21] = 0xF8;
    w16(bs + 24, 63); w16(bs + 26, 255);                    /* sectors per track, heads */
    uint32_t volid = 0x51C0FA7 + (uint32_t)total;
    if (bits == 32) {
        w32(bs + 36, fat_sectors);
        w32(bs + 44, 2);                                    /* root cluster */
        w16(bs + 48, 1); w16(bs + 50, 6);                   /* FSInfo, backup boot sector */
        bs[64] = 0x80; bs[66] = 0x29; w32(bs + 67, volid);
        memset(bs + 71, ' ', 11); memcpy(bs + 71, label, strlen(label) < 11 ? strlen(label) : 11);
        memcpy(bs + 82, "FAT32   ", 8);
    } else {
        w16(bs + 22, (uint16_t)fat_sectors);
        bs[36] = 0x80; bs[38] = 0x29; w32(bs + 39, volid);
        memset(bs + 43, ' ', 11); memcpy(bs + 43, label, strlen(label) < 11 ? strlen(label) : 11);
        memcpy(bs + 54, "FAT16   ", 8);
    }
    bs[510] = 0x55; bs[511] = 0xAA;
    if (put(0, bs, SS) != 0) { perror("write boot sector"); return 1; }
    if (bits == 32) {
        put(6, bs, SS);                                     /* backup boot sector */
        uint8_t fsi[SS]; memset(fsi, 0, SS);
        w32(fsi, 0x41615252); w32(fsi + 484, 0x61417272); w32(fsi + 488, clusters - 1); w32(fsi + 492, 3); w32(fsi + 508, 0xAA550000);
        put(1, fsi, SS);
    }

    /* FATs: zero, with the media/EOC entries (and the root cluster for FAT32). */
    uint8_t zero[SS]; memset(zero, 0, SS);
    for (uint32_t f = 0; f < fats; f++)
        for (uint32_t s = 0; s < fat_sectors; s++)
            if (put(reserved + f * fat_sectors + s, zero, SS) != 0) { perror("write FAT"); return 1; }
    uint8_t first[SS]; memset(first, 0, SS);
    if (bits == 32) { w32(first, 0x0FFFFFF8); w32(first + 4, 0x0FFFFFFF); w32(first + 8, 0x0FFFFFFF); }
    else { w16(first, 0xFFF8); w16(first + 2, 0xFFFF); }
    for (uint32_t f = 0; f < fats; f++) put(reserved + f * fat_sectors, first, SS);

    /* Root directory: fixed area (FAT16) or cluster 2 (FAT32), zeroed. */
    uint32_t root_start = reserved + fats * fat_sectors;
    uint32_t root_count = bits == 32 ? spc : root_sectors;
    for (uint32_t s = 0; s < root_count; s++)
        if (put(root_start + s, zero, SS) != 0) { perror("write root"); return 1; }
    close(fd);
    printf("mkfs.fat: %s: FAT%d, %u sectors, %u clusters of %u bytes, label \"%s\"\n",
           dev, bits, total, clusters, spc * SS, label);
    return 0;
}
