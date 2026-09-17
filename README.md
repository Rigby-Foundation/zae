# ZAE — ZAE All-purpose Environment

The userland for the [sic](https://github.com/Rigby-Foundation/sic) kernel: the programs that live in `/bin`
and the static files of the root filesystem. They are ordinary hosted C
programs built against [libc](https://github.com/Rigby-Foundation/musl) (musl ported to sic). Nothing here is
linked into the kernel; `make` produces `build/initrd.tar`, which
[zaeboot](https://github.com/Rigby-Foundation/zaeboot) loads next to the kernel and sic unpacks into its tmpfs
root at boot.

```
bin/      one C file per program -> /bin/<name>
rootfs/   copied verbatim into the image (/etc/motd, /usr/src/hello.c, ...)
ports/    third-party software, one directory per package with its own Makefile,
          patches/ and fetch rule; each installs into ports/<name>/build/root,
          overlaid onto the image. Currently: tcc (the Tiny C Compiler) and
          doom (doomgeneric with Freedoom Phase 1 as the game data - id's
          shareware WAD isn't redistributable; `make DOOM_WAD=none` leaves
          the data out and `doom -iwad /disk/doom.wad` uses your own).
          Video is `/dev/fb0`, input is `/dev/console` in raw scancode mode.
```

Programs: `init` (prints `/etc/motd`, sets PATH, mounts the first zaefs volume it
finds on `/disk` — the root partition on an installed system, or a freshly
formatted NVMe scratch disk in QEMU — and respawns the shell), `sh`
(builtins `cd pwd exit export help`; pipelines with `|`, redirections `< > >>`,
`&` for background jobs, `^C` interrupts the foreground job), `grep`, `wc`,
`sleep`, `kill`, `yes`, `threads` (pthreads smoke test), `ls`, `cat`, `echo` (`echo words > file`), `hello`, `uname`,
`env`, `crash` (deliberately faults), `mkfs.zaefs`/`mkfs.fat`/`mount`/`umount`
(persistent storage; `mount -t fat`), `sicinstall` (installs the running system
onto a disk: GPT with an ESP for UEFI, a raw boot partition for BIOS and a zaefs
root — one disk boots on both firmwares), `insmod`/`rmmod`/`lsmod` (kernel modules, shipped in `/lib/modules` from the
kernel build), `test` (libc/fork/exec/wait/pipes/signals/threads/filesystem/zaefs/FAT/mmap/modules/tcc/sockets self test, run by
the kernel at boot if present).

Networking: `net` (show interfaces; `net eth0 ADDR MASK [GATEWAY]`, `net dns
SERVER`), `dhcp` (one-shot DHCP client; init runs it on `eth0` at boot and it
writes `/etc/resolv.conf`), `ping`, `nc` (`nc HOST PORT`, `nc -l PORT`, `-u`
for UDP), `fetch` (HTTP GET: `fetch -o file http://host/path`), `httpd`
(`httpd -p 80 /some/dir &`, a static file server). Name resolution is musl's
own resolver over UDP, so any program using `getaddrinfo` works. In QEMU use
`-netdev user,id=n0,hostfwd=tcp::8080-:80 -device e1000,netdev=n0`: the guest
gets 10.0.2.15 from DHCP, the host is 10.0.2.2, and `curl localhost:8080`
reaches an `httpd` inside.

`tcc` runs on sic and compiles against the same musl the ZAE tools use: the port
ships musl's headers and static libs in `/usr/include` and `/usr/lib`, so
`tcc /usr/src/hello.c -o /tmp/hello && /tmp/hello` works at the shell. Output
binaries get the sic image base and OS/ABI byte; `-run` is not supported yet.

Adding a program: drop `bin/foo.c` with a normal `main` and run `make`; it
shows up as `/bin/foo`. Programs are linked statically at `0x8000000000`
(`--image-base`), because sic's user address space starts above the kernel's
4 GiB identity map.

`make ARCH=powerpc` builds the same programs for the 32-bit big-endian
PowerPC port (image base `0x10000000`, `build/powerpc/`); the tcc and doom
ports are x86-only and are skipped there (modules come from the kernel build
as on x86). `sicinstall` writes x86 boot code
and is not useful on a Mac.

## Installing on a machine

Boot the live system from a USB stick (write `zaeboot-bios.img` or a
`sicinstall`ed stick to it; UEFI and BIOS both work), then:

```
/ $ ls /dev            # find the target: sda/sdb (SATA), nvme0n1 (NVMe), hda (IDE)
/ $ sicinstall /dev/sda
```

It erases the disk, writes a GPT (ESP + `sicboot` + zaefs root), installs
zaeboot for both firmwares and copies the running system. Reboot from that
disk. The installed system still boots from the kernel + initrd; the zaefs root
partition is mounted on `/disk` for persistent storage.

## Building

Same toolchain as the kernel (clang + ld.lld, plus `tar` with USTAR support).

All four sic projects meet in a **sysroot** rather than knowing each other's
paths: `$SIC_SYSROOT`, default `~/.sic/sysroot` (or `make SYSROOT=...`).
ZAE reads the libc, the kernel's ABI headers and modules from there, so run
`make install` in [sic](https://github.com/Rigby-Foundation/sic) and
[libc](https://github.com/Rigby-Foundation/musl) first, then:

```bash
make            # build/initrd.tar
make install    # -> $SYSROOT/boot/initrd.tar, where zaeboot picks it up
```

## Contributing

Patches need a `Signed-off-by:` line (DCO 1.1). Read
[CODE_OF_CONFLICT](./CODE_OF_CONFLICT) and [CONTRIBUTING](./CONTRIBUTING).

## License

Copyright (C) 2026 Rigby Foundation. Licensed under the GNU General Public
License, version 2 only (`SPDX-License-Identifier: GPL-2.0-only`); see
`LICENSE`. Every source file carries an SPDX tag. Third-party software under `ports/` keeps its own licence
(tcc: LGPL-2.1-or-later, see `ports/tcc/NOTICE`); only the port glue is ours.

