# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# TCC (Tiny C Compiler) for sic: a C compiler that runs on the OS itself.
#
#   src/       tinycc "mob" checkout (fetched by `make fetch` if missing)
#   patches/   the port: __sic__ predefine, sic image base and OS/ABI byte,
#              static linking by default
#   build/     tcc binary, libtcc1.a, and the sysroot tree to drop into the initrd:
#                build/root/bin/tcc
#                build/root/usr/lib/tcc/{libtcc1.a,include/}   (tcc's own headers)
#                build/root/usr/include, build/root/usr/lib/   (musl headers, libc.a, crt*.o, from the sysroot)
#
# tcc itself is cross-built with clang against the sic musl from the sysroot,
# exactly like every other ZAE program. libtcc1.a is compiled with clang too.
# ARCH=x86_64 (build/, build/root) or ARCH=aarch64 (build/aarch64,
# build/root-aarch64): tcc generates code for the machine it runs on.

TCC_URL := https://github.com/TinyCC/tinycc/archive/refs/heads/mob.tar.gz

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
LLD_PREFIX  ?= $(firstword $(foreach f,lld lld@21 lld@20 llvm,$(if $(wildcard $(shell brew --prefix $(f) 2>/dev/null)/bin/ld.lld),$(shell brew --prefix $(f)),)))
ifeq ($(origin CC),default)
CC := $(LLVM_PREFIX)/bin/clang
endif
ifeq ($(origin LD),default)
LD := $(LLD_PREFIX)/bin/ld.lld
endif
ifeq ($(origin AR),default)
AR := $(LLVM_PREFIX)/bin/llvm-ar
endif
HOSTCC ?= zig cc      # any native C compiler; the Apple one on this box is broken
STRIP  ?= $(LLVM_PREFIX)/bin/llvm-strip

SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)
LIBC  := $(SYSROOT)/usr
ARCH  ?= x86_64
SRC   := src
PATCHES := $(wildcard patches/*.patch)

ifeq ($(ARCH),aarch64)
TARGET   := aarch64-linux-musl
TCC_ARCH := ARM64
LD_EMUL  := -m aarch64elf
BUILD    := build/aarch64
ROOT     := build/root-aarch64
LIBTCC1_SRC := lib-arm64.c stdatomic.c atomic.S builtin.c alloca.S alloca-bt.S armflush.c dsohandle.c
else
TARGET   := x86_64-linux-musl
TCC_ARCH := X86_64
LD_EMUL  :=
BUILD    := build
ROOT     := build/root
LIBTCC1_SRC := libtcc1.c stdatomic.c atomic.S builtin.c alloca.S alloca-bt.S va_list.c dsohandle.c
endif

CFLAGS  := --target=$(TARGET) -std=gnu11 -nostdinc -isystem $(LIBC)/include \
           -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -g0 \
           -Wno-everything -D_GNU_SOURCE
LDFLAGS := $(LD_EMUL) -static -nostdlib --image-base=0x8000000000 -z max-page-size=0x1000 -z noexecstack \
           --defsym=_DYNAMIC=0x8000000000
CRT_BEGIN := $(LIBC)/lib/crt1.o $(LIBC)/lib/crti.o
CRT_END   := $(LIBC)/lib/crtn.o
OSABI_SIC := \123

LIBTCC1_OBJ := $(patsubst %,$(BUILD)/lib/%.o,$(LIBTCC1_SRC))

.PHONY: all fetch port clean distclean

all: $(ROOT)/bin/tcc $(ROOT)/usr/lib/tcc/libtcc1.a $(ROOT)/usr/lib/tcc/runmain.o $(ROOT)/usr/include/stdio.h

fetch: $(SRC)/tcc.c
$(SRC)/tcc.c:
	curl -sSL -o tcc.tar.gz $(TCC_URL)
	rm -rf $(SRC) tinycc-mob && tar xzf tcc.tar.gz && mv tinycc-mob $(SRC) && rm tcc.tar.gz

port: $(SRC)/.sic-port
# Patched files keep a pristine backup (*.sic-orig); re-porting restores them first.
$(SRC)/.sic-port: $(SRC)/tcc.c $(PATCHES)
	@find $(SRC) -name '*.sic-orig' | while read f; do mv "$$f" "$${f%.sic-orig}"; done
	for p in $(PATCHES); do patch -p1 -d $(SRC) -b -z .sic-orig < $$p || exit 1; done
	touch $@

# tcc's predefined macros are compiled in as strings; c2str runs on the host.
$(BUILD)/c2str: $(SRC)/conftest.c
	@mkdir -p $(BUILD)
	$(HOSTCC) -DC2STR -o $@ $<

$(BUILD)/tccdefs_.h: $(SRC)/include/tccdefs.h $(BUILD)/c2str $(SRC)/.sic-port
	$(BUILD)/c2str $< $@

$(BUILD)/config.h: Makefile
	@mkdir -p $(BUILD)
	@{ echo '#define TCC_VERSION "0.9.28rc"'; \
	   echo '#define TCC_GITHASH "sic"'; \
	   echo '#define TCC_TARGET_$(TCC_ARCH) 1'; \
	   echo '#define TARGETOS_sic 1'; \
	   echo '#define CONFIG_TCC_PREDEFS 1'; \
	   echo '#define CONFIG_TCC_STATIC 1'; \
	   echo '#define CONFIG_TCC_BACKTRACE 0'; \
	   echo '#define CONFIG_TCC_BCHECK 0'; \
	   echo '#define CONFIG_TCC_SEMLOCK 0'; \
	   echo '#define CONFIG_TCCDIR "/usr/lib/tcc"'; \
	   echo '#define CONFIG_SYSROOT ""'; \
	   echo '#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include:/usr/local/include:/usr/include"'; \
	   echo '#define CONFIG_TCC_LIBPATHS "{B}:/usr/local/lib:/usr/lib"'; \
	   echo '#define CONFIG_TCC_CRTPREFIX "/usr/lib"'; \
	   echo '#define CONFIG_TCC_ELFINTERP "/nonexistent"'; \
	} > $@

$(BUILD)/tcc.o: $(SRC)/tcc.c $(BUILD)/config.h $(BUILD)/tccdefs_.h $(SRC)/.sic-port
	$(CC) $(CFLAGS) -DONE_SOURCE=1 -I$(BUILD) -I$(SRC) -c $< -o $@

$(ROOT)/bin/tcc: $(BUILD)/tcc.o
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(CRT_BEGIN) $< $(LIBC)/lib/libc.a $(wildcard $(LIBC)/lib/libcompiler_rt.a) $(CRT_END)
	@printf '$(OSABI_SIC)' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none
	@# the names build systems look for: cc and c99 are tcc, ar is tcc -ar, ld is tcc too, ranlib has nothing to do
	@mkdir -p $(ROOT)/usr/bin
	@ln -sf /bin/tcc $(ROOT)/usr/bin/cc
	@ln -sf /bin/tcc $(ROOT)/usr/bin/c99
	@printf '#!/bin/sh\n# ar: tcc has one\nexec tcc -ar "$$@"\n' > $(ROOT)/usr/bin/ar && chmod +x $(ROOT)/usr/bin/ar
	@printf '#!/bin/sh\n# ranlib: tcc -ar writes the symbol index already\nexit 0\n' > $(ROOT)/usr/bin/ranlib && chmod +x $(ROOT)/usr/bin/ranlib
	@printf '#!/bin/sh\n# ld: tcc is the linker too (configure scripts look for one)\ncase "$$1" in -v|-V|--version) echo "tcc linker (sic)"; exit 0;; esac\nexec tcc -nostdlib "$$@"\n' > $(ROOT)/usr/bin/ld && chmod +x $(ROOT)/usr/bin/ld

$(BUILD)/lib/%.o: $(SRC)/lib/% $(SRC)/.sic-port $(BUILD)/config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DTCC_TARGET_$(TCC_ARCH) -I$(BUILD) -I$(SRC) -I$(SRC)/include -c $< -o $@

$(ROOT)/usr/lib/tcc/libtcc1.a: $(LIBTCC1_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@ && $(AR) rcs $@ $^
	@rm -rf $(ROOT)/usr/lib/tcc/include && cp -R $(SRC)/include $(ROOT)/usr/lib/tcc/include

# `tcc -run` links this startup object into the in-memory program.
$(ROOT)/usr/lib/tcc/runmain.o: $(BUILD)/lib/runmain.c.o
	@mkdir -p $(dir $@)
	cp $< $@

# The sysroot tcc compiles against on sic: musl's headers and static libs.
$(ROOT)/usr/include/stdio.h: $(LIBC)/lib/libc.a
	@rm -rf $(ROOT)/usr/include && mkdir -p $(ROOT)/usr/include $(ROOT)/usr/lib
	cp -R $(LIBC)/include/. $(ROOT)/usr/include/
	cp $(LIBC)/lib/crt1.o $(LIBC)/lib/crti.o $(LIBC)/lib/crtn.o $(ROOT)/usr/lib/
	$(STRIP) -g -o $(ROOT)/usr/lib/libc.a $(LIBC)/lib/libc.a

clean:
	rm -rf $(BUILD) $(ROOT)

distclean: clean
	rm -rf $(SRC)
