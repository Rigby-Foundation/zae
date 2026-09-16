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

    memcpy(bs + 11, &bps, 2);
    bs[13] = spc;
    memcpy(bs + 14, &reserved, 2);
    bs[16] = fats;
    memcpy(bs + 17, &root_entries, 2);
    if (total < 0x10000) { uint16_t t16 = (uint16_t)total; memcpy(bs + 19, &t16, 2); } else memcpy(bs + 32, &total, 4);
    bs[21] = 0xF8;
    uint16_t spt = 63, heads = 255; memcpy(bs + 24, &spt, 2); memcpy(bs + 26, &heads, 2);
    uint32_t volid = 0x51C0FA7 + (uint32_t)total;
    if (bits == 32) {
        memcpy(bs + 36, &fat_sectors, 4);
        uint32_t root_cluster = 2; memcpy(bs + 44, &root_cluster, 4);
        uint16_t fsinfo = 1, backup = 6; memcpy(bs + 48, &fsinfo, 2); memcpy(bs + 50, &backup, 2);
        bs[64] = 0x80; bs[66] = 0x29; memcpy(bs + 67, &volid, 4);
        memset(bs + 71, ' ', 11); memcpy(bs + 71, label, strlen(label) < 11 ? strlen(label) : 11);
        memcpy(bs + 82, "FAT32   ", 8);
    } else {
        uint16_t f16 = (uint16_t)fat_sectors; memcpy(bs + 22, &f16, 2);
        bs[36] = 0x80; bs[38] = 0x29; memcpy(bs + 39, &volid, 4);
        memset(bs + 43, ' ', 11); memcpy(bs + 43, label, strlen(label) < 11 ? strlen(label) : 11);
        memcpy(bs + 54, "FAT16   ", 8);
    }
    bs[510] = 0x55; bs[511] = 0xAA;
    if (put(0, bs, SS) != 0) { perror("write boot sector"); return 1; }
    if (bits == 32) {
        put(6, bs, SS);                                     /* backup boot sector */
        uint8_t fsi[SS]; memset(fsi, 0, SS);
        uint32_t lead = 0x41615252, sig = 0x61417272, free_c = clusters - 1, next = 3, trail = 0xAA550000;
        memcpy(fsi, &lead, 4); memcpy(fsi + 484, &sig, 4); memcpy(fsi + 488, &free_c, 4); memcpy(fsi + 492, &next, 4); memcpy(fsi + 508, &trail, 4);
        put(1, fsi, SS);
    }

    /* FATs: zero, with the media/EOC entries (and the root cluster for FAT32). */
    uint8_t zero[SS]; memset(zero, 0, SS);
    for (uint32_t f = 0; f < fats; f++)
        for (uint32_t s = 0; s < fat_sectors; s++)
            if (put(reserved + f * fat_sectors + s, zero, SS) != 0) { perror("write FAT"); return 1; }
    uint8_t first[SS]; memset(first, 0, SS);
    if (bits == 32) { uint32_t e[3] = { 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF }; memcpy(first, e, 12); }
    else { uint16_t e[2] = { 0xFFF8, 0xFFFF }; memcpy(first, e, 4); }
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
