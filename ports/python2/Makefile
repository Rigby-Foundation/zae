# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# Python 2.7.13 for sic. Cross-built with clang against the sic musl, every
# extension module linked into the one static binary (there are no shared
# objects on sic), the standard library pruned of what can't run here.
#
#   src/          the Python-2.7.13 tarball (fetched by `make fetch` if missing)
#   patches/      the port
#   port/         Setup.local: the extension modules to build in
#   build/host/   a native python2.7, needed to build (bytecode, sysconfig)
#   build/target/ the cross build
#   build/root/   what goes into the image: bin/python2, usr/lib/python2.7
#
# Needs libz.a in the sysroot (ports/zlib).

PY_VERSION := 2.7.13
PY_URL := https://www.python.org/ftp/python/$(PY_VERSION)/Python-$(PY_VERSION).tgz

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
LLD_PREFIX  ?= $(firstword $(foreach f,lld lld@21 lld@20 llvm,$(if $(wildcard $(shell brew --prefix $(f) 2>/dev/null)/bin/ld.lld),$(shell brew --prefix $(f)),)))
CLANG  := $(LLVM_PREFIX)/bin/clang
LLD    := $(LLD_PREFIX)/bin/ld.lld
AR     := $(LLVM_PREFIX)/bin/llvm-ar
RANLIB := $(LLVM_PREFIX)/bin/llvm-ranlib
STRIP  := $(LLVM_PREFIX)/bin/llvm-strip
HOSTCC ?= zig cc      # any native C compiler; the Apple one on this box is broken

SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)
LIBC  := $(SYSROOT)/usr
SRC   := $(CURDIR)/src
BUILD := $(CURDIR)/build
HOST  := $(BUILD)/host
# ARCH: x86_64 (default) or aarch64; the host python is shared, the cross
# build and the staged tree are per architecture.
ARCH  ?= x86_64
ARCH_SUFFIX := $(if $(filter x86_64,$(ARCH)),,-$(ARCH))
TARGET := $(ARCH)-linux-musl
TGT   := $(BUILD)/target$(ARCH_SUFFIX)
ROOT  := $(BUILD)/root$(ARCH_SUFFIX)
PATCHES := $(wildcard patches/*.patch)
OSABI_SIC := \123

# The compiler is the whole toolchain here: configure and the Makefile link
# through $(CC), so the crt objects and libc ride along in LDFLAGS/LIBS.
TARGET_CC := $(CLANG) --target=$(TARGET) -nostdinc -isystem $(LIBC)/include \
             -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables -Wno-everything -D_GNU_SOURCE
TARGET_LDFLAGS := -static -no-pie -nostdlib -fuse-ld=$(LLD) -L$(LIBC)/lib \
                  -Wl,--image-base=0x8000000000 -Wl,-z,max-page-size=0x1000 -Wl,-z,noexecstack -Wl,--defsym=_DYNAMIC=0x8000000000 \
                  $(LIBC)/lib/crt1.o $(LIBC)/lib/crti.o
TARGET_LIBS := $(LIBC)/lib/libc.a $(wildcard $(LIBC)/lib/libcompiler_rt.a) $(LIBC)/lib/crtn.o

HOST_PY := $(HOST)/python.exe
# the host python running the source tree's library (no site, no environment)
RUN_HOST_PY := PYTHONPATH=$(SRC)/Lib $(HOST_PY) -E -S

# Standard library directories that make no sense on sic or are dead weight.
LIB_PRUNE := test lib-tk idlelib lib2to3 bsddb ensurepip plat-aix3 plat-aix4 plat-atheos plat-beos5 plat-darwin \
             plat-freebsd4 plat-freebsd5 plat-freebsd6 plat-freebsd7 plat-freebsd8 plat-generic plat-irix5 plat-irix6 \
             plat-mac plat-netbsd1 plat-next3 plat-os2emx plat-riscos plat-sunos5 plat-unixware7 plat-linux2/regen \
             curses sqlite3 msilib site-packages/README

.PHONY: all fetch port host target stage clean distclean

all: $(BUILD)/.staged$(ARCH_SUFFIX)

fetch: $(SRC)/configure.ac
$(SRC)/configure.ac:
	curl -sSL -o py.tgz $(PY_URL)
	rm -rf $(SRC) Python-$(PY_VERSION) && tar xzf py.tgz && mv Python-$(PY_VERSION) $(SRC) && rm py.tgz

port: $(SRC)/.sic-port
# Patched files keep a pristine backup (*.sic-orig); re-porting restores them first.
$(SRC)/.sic-port: $(SRC)/configure.ac $(PATCHES)
	@find $(SRC) -name '*.sic-orig' | while read f; do mv "$$f" "$${f%.sic-orig}"; done
	for p in $(PATCHES); do patch -p1 -d $(SRC) -b -z .sic-orig < $$p || exit 1; done
	touch $@

# ---- the host python --------------------------------------------------------------
host: $(HOST_PY)
$(HOST)/Makefile: $(SRC)/.sic-port
	@mkdir -p $(HOST)
	cd $(HOST) && CC="$(HOSTCC)" $(SRC)/configure --disable-shared --without-ensurepip --prefix=/nonexistent > configure.log 2>&1
	cp port/Setup.host $(HOST)/Modules/Setup.local
# makesetup folds Setup.local into the Makefile; that has to happen before the
# build. Serial builds: math and cmath both own Modules/_math.o (as upstream).
$(HOST_PY): $(HOST)/Makefile port/Setup.host
	cp port/Setup.host $(HOST)/Modules/Setup.local && touch $(HOST)/Modules/Setup.local
	@mkdir -p $(HOST)/lib/python2.7/lib-dynload    # keeps getpath quiet about exec_prefix
	$(MAKE) -C $(HOST) -W Modules/Setup.local Makefile > /dev/null 2>&1
	$(MAKE) -j1 -C $(HOST) python.exe LINKFORSHARED="-Wl,-u,_PyMac_Error -framework CoreFoundation" \
	        OPT="-DNDEBUG -O2 -Wno-date-time" > $(HOST)/make.log 2>&1 || { tail -30 $(HOST)/make.log; exit 1; }

# ---- the sic python ---------------------------------------------------------------
target: $(TGT)/python.exe
$(TGT)/Makefile: $(SRC)/.sic-port $(LIBC)/lib/libz.a
	@mkdir -p $(TGT)
	cd $(TGT) && CC="$(TARGET_CC)" AR=$(AR) RANLIB=$(RANLIB) LDFLAGS="$(TARGET_LDFLAGS)" LIBS="$(TARGET_LIBS)" \
	    $(SRC)/configure --host=$(TARGET) --build=aarch64-apple-darwin --disable-shared --disable-ipv6 \
	    --without-ensurepip --prefix=/usr ac_cv_file__dev_ptmx=no ac_cv_file__dev_ptc=no > configure.log 2>&1 || { tail -30 $(TGT)/configure.log; exit 1; }
# The grammar tables are shipped pre-generated; pgen would be a sic binary we can't run.
	touch $(SRC)/Include/graminit.h $(SRC)/Python/graminit.c
$(TGT)/python.exe: $(TGT)/Makefile port/Setup.local $(HOST_PY)
	cp port/Setup.local $(TGT)/Modules/Setup.local && touch $(TGT)/Modules/Setup.local
	$(MAKE) -C $(TGT) -W Modules/Setup.local Makefile > /dev/null 2>&1
	$(MAKE) -j1 -C $(TGT) python.exe BUILDEXE=.exe OPT="-DNDEBUG -O2" > $(TGT)/make.log 2>&1 || { grep -E "error|Error" $(TGT)/make.log | head -30; tail -15 $(TGT)/make.log; exit 1; }
	cd $(TGT) && _PYTHON_PROJECT_BASE=$(TGT) _PYTHON_HOST_PLATFORM=linux-$(ARCH) $(RUN_HOST_PY) -m sysconfig --generate-posix-vars

$(LIBC)/lib/libz.a:
	$(MAKE) -C ../zlib ARCH=$(ARCH) install

# ---- the image --------------------------------------------------------------------
PYLIB := $(ROOT)/usr/lib/python2.7
stage: $(BUILD)/.staged$(ARCH_SUFFIX)
$(BUILD)/.staged$(ARCH_SUFFIX): $(TGT)/python.exe port/sictest.py
	rm -rf $(ROOT)
	@mkdir -p $(ROOT)/bin $(PYLIB)/lib-dynload $(PYLIB)/site-packages
	cp $(TGT)/python.exe $(ROOT)/bin/python2         # one name: the initrd has no symlinks
	$(STRIP) $(ROOT)/bin/python2
	@printf '$(OSABI_SIC)' | dd of=$(ROOT)/bin/python2 bs=1 seek=7 count=1 conv=notrunc status=none
	cp -R $(SRC)/Lib/. $(PYLIB)/
	cd $(PYLIB) && rm -rf $(LIB_PRUNE)
	cp $(TGT)/`cat $(TGT)/pybuilddir.txt`/_sysconfigdata.py $(PYLIB)/
	cp port/sictest.py $(PYLIB)/               # `python2 -m sictest`
	@mkdir -p $(PYLIB)/config
	cp $(TGT)/Makefile $(PYLIB)/config/Makefile
	@mkdir -p $(ROOT)/usr/include/python2.7 $(LIBC)/include/python2.7 $(LIBC)/lib
	cp $(TGT)/pyconfig.h $(SRC)/Include/*.h $(ROOT)/usr/include/python2.7/
# for ports that embed Python (renpy): the headers and libpython in the sysroot
	cp $(TGT)/pyconfig.h $(SRC)/Include/*.h $(LIBC)/include/python2.7/
	cp $(TGT)/libpython2.7.a $(LIBC)/lib/libpython2.7.a
	$(RUN_HOST_PY) -m compileall -q -f -d /usr/lib/python2.7 $(PYLIB) > /dev/null || true
	@touch $@
	@echo "python2: `du -sh $(ROOT) | cut -f1` staged"

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(SRC)
