/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* reboot [MODE]: restart, or with a mode the machine understands (on a
 * Qualcomm phone: "bootloader" stops in fastboot, "recovery"). poweroff
 * if called as poweroff. reboot -n MODE only sets the reason (a test). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!(argc > 2 && strcmp(argv[1], "-n") == 0))
        umount("/mnt/data");                    /* a phone's ext4 Data: left clean */
    sync();
    if (strcmp(name, "poweroff") == 0) reboot(RB_POWER_OFF);
    else if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        if (syscall(SYS_reboot, 0xfee1dead, 672274793, 0x5ee1dead, argv[2]) != 0) { perror("reboot -n"); return 1; }
        printf("restart reason set to %s (see the kernel log)\n", argv[2]);
        return 0;
    }
    else if (argc > 1) syscall(SYS_reboot, 0xfee1dead, 672274793, 0xa1b2c3d4, argv[1]);
    else reboot(RB_AUTOBOOT);
    perror("reboot");
    return 1;
}
