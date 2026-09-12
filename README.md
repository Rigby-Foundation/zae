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
          overlaid onto the image. Currently: tcc (the Tiny C Compiler).
```

Programs: `init` (prints `/etc/motd`, sets PATH, mounts the zaefs disk on `/disk` —
formatting it with `mkfs.zaefs` on first boot — and respawns the shell), `sh`
(builtins `cd pwd exit export help`, runs commands via `execvp`, `&` for
background jobs), `ls`, `cat`, `echo` (`echo words > file`), `hello`, `uname`,
`env`, `crash` (deliberately faults), `mkfs.zaefs`/`mount`/`umount` (persistent
storage), `insmod`/`rmmod`/`lsmod` (kernel modules, shipped in `/lib/modules` from the
kernel build), `test` (libc/fork/exec/wait/filesystem/mmap/modules/tcc self test, run by
the kernel at boot if present).

`tcc` runs on sic and compiles against the same musl the ZAE tools use: the port
ships musl's headers and static libs in `/usr/include` and `/usr/lib`, so
`tcc /usr/src/hello.c -o /tmp/hello && /tmp/hello` works at the shell. Output
binaries get the sic image base and OS/ABI byte; `-run` is not supported yet.

Adding a program: drop `bin/foo.c` with a normal `main` and run `make`; it
shows up as `/bin/foo`. Programs are linked statically at `0x8000000000`
(`--image-base`), because sic's user address space starts above the kernel's
4 GiB identity map.

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

