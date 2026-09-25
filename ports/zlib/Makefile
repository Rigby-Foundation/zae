# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# zlib for sic: a static libz.a + zlib.h into the sysroot, for the ports that
# need it (python2). Nothing goes into the image; it is a build dependency.

ZLIB_VERSION := 1.3.1
ZLIB_URL := https://github.com/madler/zlib/releases/download/v$(ZLIB_VERSION)/zlib-$(ZLIB_VERSION).tar.gz

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
ifeq ($(origin CC),default)
CC := $(LLVM_PREFIX)/bin/clang
endif
ifeq ($(origin AR),default)
AR := $(LLVM_PREFIX)/bin/llvm-ar
endif

SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)
LIBC  := $(SYSROOT)/usr
ARCH  ?= x86_64
SRC   := src
BUILD := build/$(ARCH)

ifeq ($(ARCH),powerpc)
TARGET := powerpc-linux-musl
ARCH_CFLAGS := -mcpu=7450 -fno-pic -fno-pie
else ifeq ($(ARCH),aarch64)
TARGET := aarch64-linux-musl
ARCH_CFLAGS := -fPIE
else
TARGET := x86_64-linux-musl
ARCH_CFLAGS := -fPIE
endif
CFLAGS := --target=$(TARGET) -std=gnu11 -nostdinc -isystem $(LIBC)/include $(ARCH_CFLAGS) \
          -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -g0 -DHAVE_UNISTD_H -DHAVE_STDARG_H -D_LARGEFILE64_SOURCE=1 -Wno-everything

SRCS := adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))
LIB  := $(BUILD)/libz.a

.PHONY: all fetch install clean distclean
all: install

fetch: $(SRC)/zlib.h
$(SRC)/zlib.h:
	curl -sSL -o zlib.tar.gz $(ZLIB_URL)
	rm -rf $(SRC) zlib-$(ZLIB_VERSION) && tar xzf zlib.tar.gz && mv zlib-$(ZLIB_VERSION) $(SRC) && rm zlib.tar.gz

$(BUILD)/%.o: $(SRC)/%.c $(SRC)/zlib.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	rm -f $@ && $(AR) rcs $@ $^

# The ZAE image build calls `make` in every port: a library port has nothing
# to add to the image (build/root stays empty), it installs into the sysroot.
install: $(LIB)
	@mkdir -p $(SYSROOT)/usr/include $(SYSROOT)/usr/lib build/root
	@for f in $(SRC)/zlib.h $(SRC)/zconf.h; do cmp -s $$f $(SYSROOT)/usr/include/`basename $$f` || cp $$f $(SYSROOT)/usr/include/; done
	@cmp -s $(LIB) $(SYSROOT)/usr/lib/libz.a || cp $(LIB) $(SYSROOT)/usr/lib/libz.a   # only when changed: python2 reconfigures on it

clean:
	rm -rf build

distclean: clean
	rm -rf $(SRC)
