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
SRC   := src
BUILD := build
ROOT  := $(BUILD)/root
PATCHES := $(wildcard patches/*.patch)

CFLAGS  := --target=x86_64-linux-musl -std=gnu11 -nostdinc -isystem $(LIBC)/include \
           -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -g0 \
           -Wno-everything -D_GNU_SOURCE
LDFLAGS := -static -nostdlib --image-base=0x8000000000 -z max-page-size=0x1000 -z noexecstack \
           --defsym=_DYNAMIC=0x8000000000
CRT_BEGIN := $(LIBC)/lib/crt1.o $(LIBC)/lib/crti.o
CRT_END   := $(LIBC)/lib/crtn.o
OSABI_SIC := \123

LIBTCC1_SRC := libtcc1.c stdatomic.c atomic.S builtin.c alloca.S alloca-bt.S va_list.c dsohandle.c
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
	   echo '#define TCC_TARGET_X86_64 1'; \
	   echo '#define TARGETOS_sic 1'; \
	   echo '#define CONFIG_TCC_PREDEFS 1'; \
	   echo '#define CONFIG_TCC_STATIC 1'; \
	   echo '#define CONFIG_TCC_BACKTRACE 0'; \
	   echo '#define CONFIG_TCC_BCHECK 0'; \
	   echo '#define CONFIG_TCC_SEMLOCK 0'; \
	   echo '#define CONFIG_TCCDIR "/usr/lib/tcc"'; \
	   echo '#define CONFIG_SYSROOT ""'; \
	   echo '#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include:/usr/include"'; \
	   echo '#define CONFIG_TCC_LIBPATHS "{B}:/usr/lib"'; \
	   echo '#define CONFIG_TCC_CRTPREFIX "/usr/lib"'; \
	   echo '#define CONFIG_TCC_ELFINTERP "/nonexistent"'; \
	} > $@

$(BUILD)/tcc.o: $(SRC)/tcc.c $(BUILD)/config.h $(BUILD)/tccdefs_.h $(SRC)/.sic-port
	$(CC) $(CFLAGS) -DONE_SOURCE=1 -I$(BUILD) -I$(SRC) -c $< -o $@

$(ROOT)/bin/tcc: $(BUILD)/tcc.o
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(CRT_BEGIN) $< $(LIBC)/lib/libc.a $(CRT_END)
	@printf '$(OSABI_SIC)' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none

$(BUILD)/lib/%.o: $(SRC)/lib/% $(SRC)/.sic-port $(BUILD)/config.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DTCC_TARGET_X86_64 -I$(BUILD) -I$(SRC) -I$(SRC)/include -c $< -o $@

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
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(SRC)
