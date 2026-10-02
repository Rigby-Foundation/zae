/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* sicinstall: put sic on a disk so it boots on both UEFI and legacy BIOS.
 *
 * Layout (GPT with a protective MBR that carries the BIOS stage 1):
 *   LBA 0        MBR: zaeboot stage 1 + protective partition entry
 *   LBA 1..33    GPT header and entries
 *   LBA 34..     zaeboot stage 2 (BIOS), patched with the LBAs below
 *   p1  ESP      FAT32: EFI/BOOT/BOOTX64.EFI, sic.elf, initrd.tar           (UEFI boots this)
 *   p2  sicboot  raw: sic.elf then initrd.tar                                (BIOS stage 2 reads this)
 *   p3  root     zaefs, the rest: a copy of the running root filesystem     (init mounts it on /disk)
 * The first two are sized for the kernel and initrd being installed (a
 * desktop with a game bundled makes a big initrd), with room to spare.
 *
 * usage: sicinstall [--yes] /dev/<disk>
 *        sicinstall --update /dev/<disk> [kernel initrd]
 * --update puts another kernel and initrd (default: the running ones) onto
 * a disk installed before, into p1 and p2 and stage 2's table; the root
 * filesystem stays as it is. For trying a new build without a USB stick.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/random.h>

#define ESP_START   2048ULL
#define MIB         2048ULL                 /* sectors */

static uint64_t ESP_SECTORS, BOOT_SECTORS;  /* from the kernel and initrd, in main() */
#define STAGE2_LBA  34ULL
#define GPT_ENTRIES 128

static const char *disk_path;
static int disk_fd;
static uint64_t total_sectors;

static void die(const char *what)
{
    fprintf(stderr, "sicinstall: %s: %s\n", what, strerror(errno));
    exit(1);
}

/* ---- helpers ------------------------------------------------------------------- */

static void write_at(uint64_t lba, const void *buf, size_t len)
{
    if (lseek(disk_fd, (off_t)(lba * 512), SEEK_SET) < 0) die("lseek");
    if (write(disk_fd, buf, len) != (ssize_t)len) die("write");
}

static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void random_guid(uint8_t g[16])
{
    if (getrandom(g, 16, 0) != 16) die("getrandom");
    g[7] = (g[7] & 0x0F) | 0x40;
    g[8] = (g[8] & 0x3F) | 0x80;
}

/* GUID text -> the mixed-endian on-disk form. */
/* MBR and GPT are little-endian on disk whatever the CPU. */
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void parse_guid(const char *s, uint8_t g[16])
{
    unsigned v[10];
    sscanf(s, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    g[0] = v[3]; g[1] = v[2]; g[2] = v[1]; g[3] = v[0];
    g[4] = v[5]; g[5] = v[4];
    g[6] = v[7]; g[7] = v[6];
    g[8] = v[8]; g[9] = v[9];
    const char *tail = strrchr(s, '-') + 1;
    for (int i = 0; i < 6; i++) { unsigned b; sscanf(tail + i * 2, "%2x", &b); g[10 + i] = (uint8_t)b; }
}

static void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "r");
    if (!f) die(path);
    size_t cap = 1 << 20, n = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (n == cap) { cap *= 2; buf = realloc(buf, cap); }
        size_t r = fread(buf + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return buf;
}

static int run(const char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) { execv(argv[0], (char *const *)argv); _exit(127); }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static void part_name(char *out, int index)
{
    size_t n = strlen(disk_path);
    strcpy(out, disk_path);
    if (disk_path[n - 1] >= '0' && disk_path[n - 1] <= '9') out[n++] = 'p';
    out[n++] = (char)('0' + index);
    out[n] = 0;
}

static void copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "r"), *out = fopen(dst, "w");
    if (!in) die(src);
    if (!out) die(dst);
    static char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) die(dst);
    fclose(in);
    fclose(out);
}

/* Recursive copy of the live root filesystem, skipping runtime directories. */
static void copy_tree(const char *src, const char *dst, int depth)
{
    static const char *const skip[] = { "/dev", "/proc", "/tmp", "/mnt", "/disk", "/sys", "/games", NULL };   /* games: in the initrd already */
    for (int i = 0; skip[i]; i++)
        if (strcmp(src, skip[i]) == 0) return;
    DIR *d = opendir(src);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char s[512], t[512];
        snprintf(s, sizeof(s), "%s%s%s", src, strcmp(src, "/") == 0 ? "" : "/", e->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
        struct stat st;
        if (stat(s, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            mkdir(t, 0755);
            copy_tree(s, t, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            copy_file(s, t);
        }
    }
    closedir(d);
}

/* ---- the steps ------------------------------------------------------------------ */

static void write_tables(size_t stage2_sectors, uint64_t root_end)
{
    uint8_t mbr[512];
    size_t s1len;
    uint8_t *stage1 = read_file("/boot/zaeboot/stage1.bin", &s1len);
    if (s1len != 512) { fprintf(stderr, "stage1.bin is not 512 bytes\n"); exit(1); }
    memcpy(mbr, stage1, 446);
    memset(mbr + 446, 0, 64);
    uint32_t lba32 = (uint32_t)STAGE2_LBA;
    uint16_t cnt16 = (uint16_t)stage2_sectors;
    w32(mbr + 432, lba32);
    w16(mbr + 436, cnt16);
    /* protective partition: type 0xEE from LBA 1 to the end (capped) */
    uint8_t *pe = mbr + 446;
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;      /* CHS start 0/0/2 */
    pe[4] = 0xEE;
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;
    uint32_t pstart = 1, psize = total_sectors - 1 > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)(total_sectors - 1);
    w32(pe + 8, pstart);
    w32(pe + 12, psize);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    write_at(0, mbr, 512);

    /* GPT entries */
    static uint8_t entries[GPT_ENTRIES * 128];
    memset(entries, 0, sizeof(entries));
    struct { const char *type; uint64_t first, last; const char *name; } parts[3] = {
        { "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", ESP_START, ESP_START + ESP_SECTORS - 1, "EFI System" },
        { "5A4142F0-B007-4B49-9C1C-53494342F00F", ESP_START + ESP_SECTORS, ESP_START + ESP_SECTORS + BOOT_SECTORS - 1, "sicboot" },
        { "5A41524F-0007-4B49-9C1C-53494352F00F", ESP_START + ESP_SECTORS + BOOT_SECTORS, root_end, "sicroot" },
    };
    for (int i = 0; i < 3; i++) {
        uint8_t *e = entries + i * 128;
        parse_guid(parts[i].type, e);
        random_guid(e + 16);
        w64(e + 32, parts[i].first);
        w64(e + 40, parts[i].last);
        for (size_t k = 0; parts[i].name[k] && k < 36; k++) e[56 + k * 2] = (uint8_t)parts[i].name[k];
    }
    uint32_t ecrc = crc32(entries, sizeof(entries));

    uint8_t hdr[512];
    memset(hdr, 0, 512);
    memcpy(hdr, "EFI PART", 8);
    uint32_t rev = 0x00010000, hsize = 92, zero = 0;
    uint64_t cur = 1, backup = total_sectors - 1, first = 34, last = total_sectors - 34, elba = 2;
    uint32_t ecount = GPT_ENTRIES, esize = 128;
    w32(hdr + 8, rev); w32(hdr + 12, hsize); w32(hdr + 16, zero);
    w64(hdr + 24, cur); w64(hdr + 32, backup);
    w64(hdr + 40, first); w64(hdr + 48, last);
    random_guid(hdr + 56);
    w64(hdr + 72, elba); w32(hdr + 80, ecount); w32(hdr + 84, esize);
    w32(hdr + 88, ecrc);
    uint32_t hcrc = crc32(hdr, 92);
    w32(hdr + 16, hcrc);
    write_at(1, hdr, 512);
    write_at(2, entries, sizeof(entries));

    /* backup: entries just before the last sector, header in the last sector */
    uint64_t bentries = total_sectors - 33;
    w64(hdr + 24, backup); w64(hdr + 32, cur); w64(hdr + 72, bentries);
    w32(hdr + 16, zero);
    hcrc = crc32(hdr, 92);
    w32(hdr + 16, hcrc);
    write_at(bentries, entries, sizeof(entries));
    write_at(backup, hdr, 512);
}

/* Stage 2 with the sicboot partition's LBAs (kernel at `start`, the
 * initrd right after it), written where the MBR points. */
static void write_stage2(uint64_t start, uint32_t ksec, uint32_t isec, size_t *s2sec_out)
{
    size_t s2len;
    uint8_t *stage2 = read_file("/boot/zaeboot/stage2.bin", &s2len);
    uint8_t *tbl = memmem(stage2, s2len, "ZAEBIMG\0", 8);
    for (uint8_t *p = tbl; p && !(p[8] == 0 && p[9] == 0 && p[10] == 0 && p[11] == 0);)
        p = tbl = memmem(p + 1, s2len - (p + 1 - stage2), "ZAEBIMG\0", 8);
    if (!tbl) { fprintf(stderr, "stage2.bin has no image table\n"); exit(1); }
    uint32_t v[4] = { (uint32_t)start, ksec, (uint32_t)start + ksec, isec };
    memcpy(tbl + 8, v, 16);
    size_t s2sec = (s2len + 511) / 512;
    if (STAGE2_LBA + s2sec > ESP_START) { fprintf(stderr, "stage2 too large\n"); exit(1); }
    write_at(STAGE2_LBA, stage2, s2len);
    free(stage2);
    if (s2sec_out) *s2sec_out = s2sec;
}

/* sicboot: the kernel, then the initrd from its first whole sector, raw.
 * Streamed: the initrd can be most of the memory. */
static void write_sicboot(const char *p2, const char *kernel_path, const char *initrd_path, uint32_t ksec, size_t ilen)
{
    int bfd = open(p2, O_WRONLY);
    if (bfd < 0) die(p2);
    static char buf[256 * 1024];
    const char *src[2] = { kernel_path, initrd_path };
    for (int i = 0; i < 2; i++) {
        if (i == 1 && lseek(bfd, (off_t)ksec * 512, SEEK_SET) < 0) die("lseek");
        int fd = open(src[i], O_RDONLY);
        if (fd < 0) die(src[i]);
        ssize_t n;
        size_t done = 0;
        while ((n = read(fd, buf, sizeof buf)) > 0) {
            if (write(bfd, buf, (size_t)n) != n) die(i ? "write initrd" : "write kernel");
            done += (size_t)n;
        }
        close(fd);
        if (i == 1 && done != ilen) { fprintf(stderr, "sicinstall: %s: short read\n", src[i]); exit(1); }
    }
    close(bfd);
}

/* First LBA and size (sectors) of GPT partition `index` (1-based). */
static void gpt_part(int index, uint64_t *first, uint64_t *sectors)
{
    uint8_t e[128];
    if (lseek(disk_fd, (off_t)(2 * 512 + (index - 1) * 128), SEEK_SET) < 0 || read(disk_fd, e, 128) != 128) die("read GPT");
    uint64_t f = 0, l = 0;
    for (int i = 7; i >= 0; i--) { f = f << 8 | e[32 + i]; l = l << 8 | e[40 + i]; }
    if (!f || l < f) { fprintf(stderr, "sicinstall: %s: no partition %d; install first\n", disk_path, index); exit(1); }
    *first = f; *sectors = l - f + 1;
}

static int update(const char *kernel_path, const char *initrd_path)
{
    struct stat ks, is;
    if (stat(kernel_path, &ks) != 0) die(kernel_path);
    if (stat(initrd_path, &is) != 0) die(initrd_path);
    disk_fd = open(disk_path, O_RDWR);
    if (disk_fd < 0) die(disk_path);
    uint8_t hdr[8];
    if (lseek(disk_fd, 512, SEEK_SET) < 0 || read(disk_fd, hdr, 8) != 8 || memcmp(hdr, "EFI PART", 8) != 0) {
        fprintf(stderr, "sicinstall: %s has no GPT: not an installed disk\n", disk_path);
        return 1;
    }
    uint64_t esp_first, esp_sectors, boot_first, boot_sectors;
    gpt_part(1, &esp_first, &esp_sectors);
    gpt_part(2, &boot_first, &boot_sectors);
    uint32_t ksec = (uint32_t)((ks.st_size + 511) / 512), isec = (uint32_t)((is.st_size + 511) / 512);
    if (ksec + isec > boot_sectors || (uint64_t)(ks.st_size + is.st_size) / 512 + 8 * MIB > esp_sectors) {
        fprintf(stderr, "sicinstall: the new kernel and initrd (%llu MiB) do not fit the boot partitions (%llu MiB); reinstall\n",
                (unsigned long long)((ks.st_size + is.st_size) >> 20), (unsigned long long)(boot_sectors / MIB));
        return 1;
    }
    char p1[64], p2[64];
    part_name(p1, 1); part_name(p2, 2);
    printf("sicinstall: updating %s: kernel %lld KiB, initrd %lld MiB\n", disk_path, (long long)ks.st_size >> 10, (long long)is.st_size >> 20);
    printf("  writing %s (BIOS boot)\n", p2);
    write_sicboot(p2, kernel_path, initrd_path, ksec, (size_t)is.st_size);
    write_stage2(boot_first, ksec, isec, NULL);            /* after the data it points at */
    close(disk_fd);
    printf("  writing the files on %s (UEFI boot)\n", p1);
    mkdir("/mnt/esp", 0755);
    if (mount(p1, "/mnt/esp", "fat", 0, NULL) != 0) die("mount ESP");
    copy_file(kernel_path, "/mnt/esp/sic.elf");
    copy_file(initrd_path, "/mnt/esp/initrd.tar");
    if (umount("/mnt/esp") != 0) die("umount ESP");
    sync();
    printf("sicinstall: updated. Reboot to run it.\n");
    return 0;
}

int main(int argc, char **argv)
{
    int yes = 0, upd = 0;
    const char *files[2] = { "/boot/sic.elf", "/dev/initrd" };
    int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--yes") == 0) yes = 1;
        else if (strcmp(argv[i], "--update") == 0) upd = 1;
        else if (!disk_path) disk_path = argv[i];
        else if (upd && nfiles < 2) files[nfiles++] = argv[i];
    }
    if (!disk_path || (upd && nfiles == 1)) {
        fprintf(stderr, "usage: sicinstall [--yes] /dev/<disk>\n"
                        "       sicinstall --update /dev/<disk> [kernel initrd]\n");
        return 2;
    }
    if (upd) return update(files[0], files[1]);
    crc_init();
    /* the boot partitions: the kernel and initrd, plus room for a bigger
     * system later (FAT and the loader too, on the ESP) */
    struct stat ks, is;
    if (stat("/boot/sic.elf", &ks) != 0) die("/boot/sic.elf");
    if (stat("/dev/initrd", &is) != 0) die("/dev/initrd");
    uint64_t need = ((uint64_t)ks.st_size + (uint64_t)is.st_size + 511) / 512;
    BOOT_SECTORS = (need + need / 4 + 16 * MIB) / MIB * MIB;
    ESP_SECTORS = BOOT_SECTORS + 16 * MIB;
    if (ESP_SECTORS < 64 * MIB) ESP_SECTORS = 64 * MIB;
    disk_fd = open(disk_path, O_RDWR);
    if (disk_fd < 0) die(disk_path);
    uint64_t bytes;
    if (ioctl(disk_fd, BLKGETSIZE64, &bytes) != 0) die("BLKGETSIZE64 (is this a whole disk?)");
    total_sectors = bytes / 512;
    if (total_sectors < ESP_START + ESP_SECTORS + BOOT_SECTORS + 64 * MIB + 34) {
        fprintf(stderr, "sicinstall: %s is too small (%llu MiB); need at least %llu MiB\n", disk_path, (unsigned long long)(bytes >> 20),
                (unsigned long long)((ESP_START + ESP_SECTORS + BOOT_SECTORS + 64 * MIB + 34) / MIB));
        return 1;
    }

    printf("sicinstall: target %s, %llu MiB. This ERASES the disk.\n", disk_path, (unsigned long long)(bytes >> 20));
    if (!yes) {
        printf("Type 'yes' to continue: ");
        fflush(stdout);
        char line[16];
        if (!fgets(line, sizeof(line), stdin) || strncmp(line, "yes", 3) != 0) { puts("aborted"); return 1; }
    }

    /* 1. stage 2 with the sicboot partition's LBAs, then the partition tables */
    size_t s2sec, ilen = (size_t)is.st_size;
    uint64_t boot_start = ESP_START + ESP_SECTORS;
    uint32_t ksec = (uint32_t)((ks.st_size + 511) / 512), isec = (uint32_t)((ilen + 511) / 512);
    printf("  writing partition tables and BIOS stages\n");
    write_stage2(boot_start, ksec, isec, &s2sec);
    write_tables(s2sec, total_sectors - 34);
    if (ioctl(disk_fd, BLKRRPART, 0) != 0) die("BLKRRPART");
    close(disk_fd);

    char p1[64], p2[64], p3[64];
    part_name(p1, 1); part_name(p2, 2); part_name(p3, 3);

    /* 2. sicboot: kernel then initrd, raw */
    printf("  writing %s (kernel + initrd for BIOS boot)\n", p2);
    write_sicboot(p2, "/boot/sic.elf", "/dev/initrd", ksec, ilen);

    /* 3. ESP */
    printf("  formatting %s as FAT32 and installing the UEFI loader\n", p1);
    const char *const mkfat[] = { "/bin/mkfs.fat", "-n", "SICBOOT", p1, NULL };
    if (run(mkfat) != 0) { fprintf(stderr, "mkfs.fat failed\n"); return 1; }
    mkdir("/mnt/esp", 0755);
    if (mount(p1, "/mnt/esp", "fat", 0, NULL) != 0) die("mount ESP");
    mkdir("/mnt/esp/EFI", 0755);
    mkdir("/mnt/esp/EFI/BOOT", 0755);
    copy_file("/boot/zaeboot/BOOTX64.EFI", "/mnt/esp/EFI/BOOT/BOOTX64.EFI");
    copy_file("/boot/sic.elf", "/mnt/esp/sic.elf");
    copy_file("/dev/initrd", "/mnt/esp/initrd.tar");
    if (umount("/mnt/esp") != 0) die("umount ESP");

    /* 4. root */
    printf("  formatting %s as zaefs and copying the system\n", p3);
    const char *const mkz[] = { "/bin/mkfs.zaefs", "-L", "sicroot", p3, NULL };
    if (run(mkz) != 0) { fprintf(stderr, "mkfs.zaefs failed\n"); return 1; }
    mkdir("/mnt/root", 0755);
    if (mount(p3, "/mnt/root", "zaefs", 0, NULL) != 0) die("mount root");
    copy_tree("/", "/mnt/root", 0);
    if (umount("/mnt/root") != 0) die("umount root");

    printf("sicinstall: done. Reboot and boot from %s (UEFI or BIOS).\n", disk_path);
    return 0;
}
