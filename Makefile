# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# ZAE - ZAE All-purpose Environment: the userland for the sic kernel.
#
# Programs are ordinary hosted C, compiled against the sic musl from the sysroot
# and linked statically at 0x8000000000 (sic's user address space starts
# above the 4 GiB kernel identity map). `make` packs bin/ + rootfs/ into
# build/initrd.tar for zaeboot to hand to the kernel.

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
LLD_PREFIX  ?= $(firstword $(foreach f,lld lld@21 lld@20 llvm,$(if $(wildcard $(shell brew --prefix $(f) 2>/dev/null)/bin/ld.lld),$(shell brew --prefix $(f)),)))

ifeq ($(origin CC),default)
CC := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/clang,clang)
endif
ifeq ($(origin LD),default)
LD := $(if $(LLD_PREFIX),$(LLD_PREFIX)/bin/ld.lld,ld.lld)
endif

# Everything from the other projects comes through the sysroot: musl headers
# and libs (libc `make install`), the kernel's ABI headers under abi/ and its
# modules (sic `make install`). The image goes back there as boot/initrd.tar.
SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)
export SYSROOT
LIBC    := $(SYSROOT)/usr

# ARCH selects the target; x86_64 is the default, powerpc is the 32-bit
# big-endian PowerMac port (user space 0-0x7FFFFFFF, image at 0x10000000).
ARCH ?= x86_64
BUILD   := build/$(ARCH)
INITRD  := $(BUILD)/initrd.tar
ifeq ($(ARCH),powerpc)
TARGET     := powerpc-linux-musl
ARCH_CFLAGS := -mcpu=7450 -maltivec -fno-pic -fno-pie
IMAGE_BASE := 0x10000000
LD_EMUL    := -m elf32ppc
# Ports carrying x86-only code (tcc, doom's fb path) are not built for ppc yet.
PORTS_SKIP := tcc doom
else
TARGET     := x86_64-linux-musl
ARCH_CFLAGS := -fPIE
IMAGE_BASE := 0x8000000000
LD_EMUL    :=
PORTS_SKIP :=
endif
export ARCH TARGET ARCH_CFLAGS IMAGE_BASE LD_EMUL

CFLAGS  := --target=$(TARGET) -std=c11 -nostdinc -isystem $(LIBC)/include \
           $(ARCH_CFLAGS) -fno-stack-protector -fno-asynchronous-unwind-tables \
           -O2 -g -Wall -Wextra -D_GNU_SOURCE
# _DYNAMIC: musl's _start takes its address with a RIP-relative lea (only
# static-PIE uses it); as an undefined weak it would resolve to 0, out of
# PC32 range from our image base, so give it an in-range dummy value.
LDFLAGS := $(LD_EMUL) -static -nostdlib --image-base=$(IMAGE_BASE) -z max-page-size=0x1000 -z noexecstack \
           --defsym=_DYNAMIC=$(IMAGE_BASE)
CRT_BEGIN := $(LIBC)/lib/crt1.o $(LIBC)/lib/crti.o
CRT_END   := $(LIBC)/lib/crtn.o
LIBS      := $(LIBC)/lib/libc.a

PROGS   := $(patsubst bin/%.c,%,$(wildcard bin/*.c))
ELFS    := $(patsubst %,$(BUILD)/bin/%,$(PROGS))
ROOTFS  := $(shell find rootfs -type f)
PORTS   := $(filter-out $(PORTS_SKIP),$(patsubst ports/%/Makefile,%,$(wildcard ports/*/Makefile)))

.PHONY: all clean ports install $(PORTS)

all: $(INITRD)

install: $(INITRD)
	@mkdir -p $(SYSROOT)/boot
	cp $(INITRD) $(SYSROOT)/boot/initrd.tar
	@echo "installed into $(SYSROOT)"

# Third-party software (ports/<name>/) each installs into ports/<name>/build/root,
# which is overlaid onto the image.
ports: $(PORTS)
$(PORTS): $(LIBC)/lib/libc.a
	$(MAKE) -C ports/$@

$(LIBC)/lib/libc.a:
	@echo "error: no libc in $(SYSROOT) (run 'make install' in the libc repo, or set SYSROOT)"; exit 1

# sic executables carry OS/ABI 0x53 ('S') in e_ident[7] (sic/include/abi/abi.h);
# the kernel refuses anything else. lld can't set it, so stamp the byte.
OSABI_SIC := \123
stamp-osabi = printf '$(OSABI_SIC)' | dd of=$(1) bs=1 seek=7 count=1 conv=notrunc status=none

$(BUILD)/bin/%: $(BUILD)/bin/%.o $(LIBC)/lib/libc.a
	$(LD) $(LDFLAGS) -o $@ $(CRT_BEGIN) $< $(LIBS) $(CRT_END)
	@$(call stamp-osabi,$@)

$(BUILD)/%.o: %.c $(LIBC)/lib/libc.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(patsubst %,$(BUILD)/bin/%.d,$(PROGS))

# Layout: /bin/<prog> plus everything under rootfs/ (etc/motd, ...); /dev and
# /tmp exist so the kernel can populate them.
$(INITRD): $(ELFS) $(ROOTFS) ports $(wildcard $(SYSROOT)/lib/modules/*.ko) $(SYSROOT)/boot/sic.elf $(wildcard $(SYSROOT)/boot/zaeboot/*)
	@rm -rf $(BUILD)/root && mkdir -p $(BUILD)/root/bin $(BUILD)/root/dev $(BUILD)/root/tmp
	@cp -R rootfs/. $(BUILD)/root/
	@cp $(ELFS) $(BUILD)/root/bin/
	@for p in $(PORTS); do cp -R ports/$$p/build/root/. $(BUILD)/root/; done
	@mkdir -p $(BUILD)/root/boot && cp $(SYSROOT)/boot/sic.elf $(BUILD)/root/boot/ && \
	    { [ -d $(SYSROOT)/boot/zaeboot ] && cp -R $(SYSROOT)/boot/zaeboot $(BUILD)/root/boot/ || echo "note: no $(SYSROOT)/boot/zaeboot (run 'zig build sysroot' in zaeboot); sicinstall won't work"; }
	@mkdir -p $(BUILD)/root/lib/modules && cp $(SYSROOT)/lib/modules/*.ko $(BUILD)/root/lib/modules/ 2>/dev/null || true
	tar --format ustar -cf $@ -C $(BUILD)/root .

clean:
	rm -rf build
	@for p in $(PORTS); do $(MAKE) -C ports/$$p clean; done
