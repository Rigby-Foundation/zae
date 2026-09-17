/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* mkfs.zaefs: format a block device (or image file) with an empty zaefs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <abi/zaefs.h>

#define BS ZAEFS_BLOCK_SIZE

static int fd;

static int put_block(uint64_t blk, const void *buf)
{
    if (lseek(fd, (off_t)(blk * BS), SEEK_SET) < 0)
        return -1;
    return write(fd, buf, BS) == BS ? 0 : -1;
}

static void set_bit(uint8_t *bitmap, uint64_t i) { bitmap[i / 8] |= (uint8_t)(1 << (i % 8)); }

int main(int argc, char **argv)
{
    const char *label = "zaefs";
    const char *dev = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) label = argv[++i];
        else dev = argv[i];
    }
    if (!dev) {
        fprintf(stderr, "usage: mkfs.zaefs [-L label] <device>\n");
        return 2;
    }
    fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }

    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0) size = st.st_size;
    }
    if (size < 64 * BS) { fprintf(stderr, "%s: too small (%lld bytes)\n", dev, (long long)size); return 1; }

    struct zaefs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = ZAEFS_MAGIC;
    sb.version = ZAEFS_VERSION;
    sb.block_size = BS;
    sb.total_blocks = (uint64_t)size / BS;
    sb.inode_count = sb.total_blocks / 4;
    if (sb.inode_count < 64) sb.inode_count = 64;
    sb.bbitmap_start = 1;
    sb.bbitmap_blocks = (sb.total_blocks + BS * 8 - 1) / (BS * 8);
    sb.ibitmap_start = sb.bbitmap_start + sb.bbitmap_blocks;
    sb.ibitmap_blocks = (sb.inode_count + BS * 8 - 1) / (BS * 8);
    sb.itable_start = sb.ibitmap_start + sb.ibitmap_blocks;
    sb.itable_blocks = (sb.inode_count * ZAEFS_INODE_SIZE + BS - 1) / BS;
    sb.data_start = sb.itable_start + sb.itable_blocks;
    sb.root_ino = ZAEFS_ROOT_INO;
    strncpy(sb.label, label, sizeof(sb.label) - 1);
    sb.free_blocks = sb.total_blocks - sb.data_start;
    sb.free_inodes = sb.inode_count - 2;            /* inode 0 unused, 1 = root */

    /* Block bitmap: metadata blocks used. */
    uint8_t *bitmap = calloc(sb.bbitmap_blocks, BS);
    for (uint64_t b = 0; b < sb.data_start; b++) set_bit(bitmap, b);
    for (uint64_t b = 0; b < sb.bbitmap_blocks; b++)
        if (put_block(sb.bbitmap_start + b, bitmap + b * BS) != 0) { perror("write"); return 1; }
    free(bitmap);

    /* Inode bitmap: 0 and root used. Inode table: zeroed, root initialised. */
    uint8_t *ibitmap = calloc(sb.ibitmap_blocks, BS);
    set_bit(ibitmap, 0);
    set_bit(ibitmap, ZAEFS_ROOT_INO);
    for (uint64_t b = 0; b < sb.ibitmap_blocks; b++)
        if (put_block(sb.ibitmap_start + b, ibitmap + b * BS) != 0) { perror("write"); return 1; }
    free(ibitmap);

    uint8_t *zero = calloc(1, BS);
    for (uint64_t b = 0; b < sb.itable_blocks; b++)
        if (put_block(sb.itable_start + b, zero) != 0) { perror("write"); return 1; }
    struct zaefs_inode *root = (struct zaefs_inode *)(zero + ZAEFS_ROOT_INO * ZAEFS_INODE_SIZE);
    root->type = ZAEFS_TYPE_DIR;
    root->links = 1;
    root->mode = 0755;
    ZAEFS_INODE_SWAP(root);                         /* the disk is little-endian */
    if (put_block(sb.itable_start, zero) != 0) { perror("write"); return 1; }
    free(zero);

    printf("mkfs.zaefs: %s: %llu blocks of %u bytes, %llu inodes, label \"%s\"\n",
           dev, (unsigned long long)sb.total_blocks, BS, (unsigned long long)sb.inode_count, sb.label);
    ZAEFS_SB_SWAP(&sb);
    if (put_block(0, &sb) != 0) { perror("write superblock"); return 1; }
    close(fd);
    return 0;
}
