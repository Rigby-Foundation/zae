# python2 — Python 2.7.13 on sic

CPython 2.7.13, cross-built against the sic musl into one static binary
with every extension module linked in (sic has no shared objects). Where
Ren'Py lives, eventually.

```
/bin/python2                 the interpreter (stripped, ~4 MB)
/usr/lib/python2.7/          the standard library (.py + .pyc), pruned
python2 -m sictest           does it work here? files, fork, threads, sockets, ...
```

What is built in (`port/Setup.local`): the usual suspects — `_io`, `_struct`,
`math`/`cmath`, `time`/`datetime`, `_socket`/`select`, `zlib`, `_json`,
`unicodedata`, `pyexpat`/`_elementtree`, `cPickle`/`cStringIO`, the hashes,
`mmap`, `fcntl`, `resource`, the CJK codecs. Not built: `_ssl`, `_sqlite3`,
`bz2`, `_curses`, `readline`, `_tkinter`, `_ctypes` (no libraries or no
`dlopen`). Pruned from the library: `test`, `lib-tk`, `idlelib`, `lib2to3`,
`bsddb`, `ensurepip`, the `plat-*` of other systems.

## Building

`make` in ZAE does it (x86_64 only for now). Two builds happen: a native
`build/host/python.exe` (any C compiler; `HOSTCC`, default `zig cc`) that
the build itself needs for bytecode and `sysconfig`, and the cross build in
`build/target`. The grammar tables ship pre-generated in the tarball, so no
`pgen` has to run; `setup.py` is never run either — the modules come from
`Setup.local`. Needs `libz.a` in the sysroot (`ports/zlib`). The one patch
teaches `configure` about arm64 Macs as a build machine.
