# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# BusyBox for sic: ash, coreutils, find/grep/sed/awk/diff/patch, vi and less,
# tar/gzip/bzip2/xz/unzip, and wget with HTTPS (BusyBox's own TLS) — what
# fetching and building software on the OS itself needs. One binary,
# /bin/busybox, and every applet as a link in /usr/bin.
#
#   src/        busybox release (fetched by `make fetch` if missing)
#   sic.config  the applets, merged into allnoconfig
#   sic-cc.in   clang for sic: links static programs the way ZAE does
#   build/<arch>/obj   the out-of-tree build; build/root[-<arch>]/bin/busybox the result

BB_VERSION := 1.37.0
BB_URL := https://busybox.net/downloads/busybox-$(BB_VERSION).tar.bz2

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
LLD_PREFIX  ?= $(firstword $(foreach f,lld lld@21 lld@20 llvm,$(if $(wildcard $(shell brew --prefix $(f) 2>/dev/null)/bin/ld.lld),$(shell brew --prefix $(f)),)))
CLANG := $(LLVM_PREFIX)/bin/clang
AR    := $(LLVM_PREFIX)/bin/llvm-ar
STRIP := $(LLVM_PREFIX)/bin/llvm-strip
HOSTCC ?= zig cc      # any native C compiler; the Apple one on this box is broken

SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)
LIBC  := $(SYSROOT)/usr
ARCH  ?= x86_64
SRC   := src
BUILD := $(CURDIR)/build/$(ARCH)
OBJ   := $(BUILD)/obj
ROOT  := build/root$(if $(filter x86_64,$(ARCH)),,-$(ARCH))

ifeq ($(ARCH),aarch64)
TARGET := aarch64-linux-musl
else
TARGET := x86_64-linux-musl
endif

BBMAKE := $(MAKE) -C $(OBJ) HOSTCC="$(HOSTCC)" CC=$(BUILD)/sic-cc AR=$(AR) STRIP=$(STRIP)

.PHONY: all fetch clean distclean
all: $(ROOT)/bin/busybox

fetch: $(SRC)/Makefile
$(SRC)/Makefile:
	curl -sSL -o bb.tar.bz2 $(BB_URL)
	rm -rf $(SRC) busybox-$(BB_VERSION) && tar xjf bb.tar.bz2 && mv busybox-$(BB_VERSION) $(SRC) && rm bb.tar.bz2

$(BUILD)/sic-cc: sic-cc.in Makefile
	@mkdir -p $(BUILD)/lib
	sed 's|@TARGET@|$(TARGET)|g; s|@LIBC@|$(LIBC)|; s|@CLANG@|$(CLANG)|; s|@LLD@|$(LLD_PREFIX)/bin/ld.lld|' $< > $@
	chmod +x $@
	@for l in m resolv crypt rt pthread; do $(AR) rcs $(BUILD)/lib/lib$$l.a; done   # all in musl's libc.a

# allnoconfig, then sic.config on top, then oldconfig for what that pulls in
$(OBJ)/.config: sic.config $(SRC)/Makefile
	@mkdir -p $(OBJ)
	$(MAKE) -C $(SRC) O=$(OBJ) HOSTCC="$(HOSTCC)" allnoconfig >/dev/null
	python3 merge-config.py $@ sic.config
	yes "" | $(MAKE) -C $(OBJ) HOSTCC="$(HOSTCC)" oldconfig >/dev/null

$(OBJ)/busybox: $(OBJ)/.config $(BUILD)/sic-cc $(LIBC)/lib/libc.a
	find $(OBJ) -name built-in.o -delete
	$(BBMAKE) busybox busybox.links
	@touch $@

$(ROOT)/bin/busybox: $(OBJ)/busybox
	@mkdir -p $(dir $@)
	cp $< $@
	@printf '\123' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none   # sic's OS/ABI byte
	@# every applet as a command: /usr/bin/<applet> -> /bin/busybox (init puts /usr/bin first in PATH)
	@rm -rf $(ROOT)/usr/bin && mkdir -p $(ROOT)/usr/bin
	@for a in $$(sed 's|.*/||' $(OBJ)/busybox.links); do [ $$a = sh ] || ln -s /bin/busybox $(ROOT)/usr/bin/$$a; done   # sh: ZAE's own

clean:
	rm -rf $(BUILD) $(ROOT)

distclean: clean
	rm -rf $(SRC)
