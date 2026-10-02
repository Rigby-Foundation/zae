# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# The C++ runtime for sic: LLVM's libc++, libc++abi and libunwind, built
# from the sources Zig ships (its lib directory carries all three, at the
# version its clang uses) against sic's musl, into the sysroot:
#   usr/include/c++/v1/            the headers, with a real __config_site
#   usr/lib/libc++.a libc++abi.a libunwind.a
# Build C++ with `-nostdinc++ -isystem $(SYSROOT)/usr/include/c++/v1` before
# the C headers, link with `-lc++ -lc++abi -lunwind`. Nothing goes into the
# image; it is a build dependency.

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
ifeq ($(origin CC),default)
CC := $(LLVM_PREFIX)/bin/clang
endif
CXX := $(LLVM_PREFIX)/bin/clang++
ifeq ($(origin AR),default)
AR := $(LLVM_PREFIX)/bin/llvm-ar
endif

# Where Zig keeps libcxx/, libcxxabi/, libunwind/
ZIG_LIB ?= $(shell zig env 2>/dev/null | sed -n 's/.*\.lib_dir = "\(.*\)",/\1/p')

SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)
LIBC  := $(SYSROOT)/usr
ARCH  ?= x86_64
BUILD := build/$(ARCH)
HDRS  := $(BUILD)/include/c++/v1

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

COMMON  := --target=$(TARGET) -nostdinc $(ARCH_CFLAGS) -O2 -g0 -fno-stack-protector \
           -ffunction-sections -fdata-sections -DNDEBUG -Wno-everything
CXXINC  := -nostdinc++ -isystem $(HDRS) -isystem $(LIBC)/include -isystem compat   # compat: linux/futex.h for atomic.cpp
# what libc++'s own build adds: building the library, with libc++abi as its ABI library
LIBFLAGS := -D_LIBCPP_BUILDING_LIBRARY -DLIBCXX_BUILDING_LIBCXXABI -DLIBC_NAMESPACE=__llvm_libc_common_utils \
            -D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS -D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS \
            -D_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER
CXX_SRCS := algorithm any atomic barrier bind call_once charconv chrono condition_variable \
            condition_variable_destructor error_category exception expected \
            filesystem/directory_entry filesystem/directory_iterator filesystem/filesystem_clock \
            filesystem/filesystem_error filesystem/operations filesystem/path fstream functional future \
            hash ios ios.instantiations iostream locale memory memory_resource mutex mutex_destructor \
            new new_handler new_helpers optional ostream print random random_shuffle regex \
            ryu/d2fixed ryu/d2s ryu/f2s shared_mutex stdexcept string strstream system_error thread \
            typeinfo valarray variant vector verbose_abort
ABI_SRCS := abort_message cxa_aux_runtime cxa_default_handlers cxa_demangle cxa_exception \
            cxa_exception_storage cxa_guard cxa_handlers cxa_personality cxa_thread_atexit cxa_vector \
            cxa_virtual fallback_malloc private_typeinfo stdlib_exception stdlib_new_delete \
            stdlib_stdexcept stdlib_typeinfo
UNW_CXX  := libunwind
UNW_C    := UnwindLevel1 UnwindLevel1-gcc-ext gcc_personality_v0
UNW_S    := UnwindRegistersRestore UnwindRegistersSave

CXX_OBJS := $(patsubst %,$(BUILD)/cxx/%.o,$(CXX_SRCS))
ABI_OBJS := $(patsubst %,$(BUILD)/abi/%.o,$(ABI_SRCS))
UNW_OBJS := $(patsubst %,$(BUILD)/unw/%.o,$(UNW_CXX) $(UNW_C) $(UNW_S))
LIBS     := $(BUILD)/libc++.a $(BUILD)/libc++abi.a $(BUILD)/libunwind.a

.PHONY: all headers install clean
all: install

# The headers: libc++'s and libc++abi's public ones, and the configuration
# as a __config_site (Zig patches __config to take -D flags instead).
headers: $(HDRS)/__config_site
$(HDRS)/__config_site: Makefile
	@test -d "$(ZIG_LIB)/libcxx" || { echo "libcxx: no Zig lib directory (ZIG_LIB=$(ZIG_LIB))"; exit 1; }
	rm -rf $(HDRS) && mkdir -p $(HDRS)
	cp -R $(ZIG_LIB)/libcxx/include/. $(HDRS)/
	cp $(ZIG_LIB)/libcxxabi/include/*.h $(HDRS)/
	cp $(ZIG_LIB)/libunwind/include/__libunwind_config.h $(ZIG_LIB)/libunwind/include/libunwind.h \
	   $(ZIG_LIB)/libunwind/include/unwind.h $(ZIG_LIB)/libunwind/include/unwind_itanium.h \
	   $(ZIG_LIB)/libunwind/include/unwind_arm_ehabi.h $(HDRS)/
	sed -i.orig 's|^/\* zig patch: instead of including __config_site, zig adds -D flags when compiling \*/|#include <__config_site>|' $(HDRS)/__config && rm -f $(HDRS)/__config.orig
	grep -q '^#include <__config_site>' $(HDRS)/__config
	printf '%s\n' '/* libc++ configured for sic (musl, static) */' '#ifndef _LIBCPP___CONFIG_SITE' '#define _LIBCPP___CONFIG_SITE' \
	  '#define _LIBCPP_ABI_VERSION 1' '#define _LIBCPP_ABI_NAMESPACE __1' \
	  '#define _LIBCPP_HAS_THREADS 1' '#define _LIBCPP_HAS_MONOTONIC_CLOCK 1' '#define _LIBCPP_HAS_MUSL_LIBC 1' \
	  '#define _LIBCPP_HAS_FILESYSTEM 1' '#define _LIBCPP_HAS_LOCALIZATION 1' '#define _LIBCPP_HAS_RANDOM_DEVICE 1' \
	  '#define _LIBCPP_HAS_TERMINAL 1' '#define _LIBCPP_HAS_UNICODE 1' '#define _LIBCPP_HAS_WIDE_CHARACTERS 1' \
	  '#define _LIBCPP_HAS_TIME_ZONE_DATABASE 0' '#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0' \
	  '#define _LIBCPP_PSTL_BACKEND_SERIAL 1' '#define _LIBCPP_HARDENING_MODE_DEFAULT _LIBCPP_HARDENING_MODE_NONE' \
	  '#endif' > $@

$(BUILD)/cxx/%.o: $(ZIG_LIB)/libcxx/src/%.cpp $(HDRS)/__config_site
	@mkdir -p $(dir $@)
	$(CXX) $(COMMON) $(CXXINC) -I$(ZIG_LIB)/libcxx/src -I$(ZIG_LIB)/libcxx/libc -std=c++23 $(LIBFLAGS) -c $< -o $@

$(BUILD)/abi/%.o: $(ZIG_LIB)/libcxxabi/src/%.cpp $(HDRS)/__config_site
	@mkdir -p $(dir $@)
	$(CXX) $(COMMON) $(CXXINC) -I$(ZIG_LIB)/libcxx/src -std=c++23 $(LIBFLAGS) -D_LIBCXXABI_BUILDING_LIBRARY -c $< -o $@

UNWFLAGS := -I$(ZIG_LIB)/libunwind/include -D_LIBUNWIND_IS_NATIVE_ONLY -D_LIBUNWIND_HIDE_SYMBOLS
$(BUILD)/unw/%.o: $(ZIG_LIB)/libunwind/src/%.cpp $(HDRS)/__config_site
	@mkdir -p $(dir $@)
	$(CXX) $(COMMON) $(CXXINC) $(UNWFLAGS) -std=c++17 -fno-exceptions -fno-rtti -c $< -o $@
$(BUILD)/unw/%.o: $(ZIG_LIB)/libunwind/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) -isystem $(LIBC)/include $(UNWFLAGS) -std=gnu11 -fexceptions -c $< -o $@
$(BUILD)/unw/%.o: $(ZIG_LIB)/libunwind/src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) -isystem $(LIBC)/include $(UNWFLAGS) -c $< -o $@

$(BUILD)/libc++.a: $(CXX_OBJS)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/libc++abi.a: $(ABI_OBJS)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/libunwind.a: $(UNW_OBJS)
	rm -f $@ && $(AR) rcs $@ $^

# The ZAE image build calls `make` in every port: a library port has nothing
# to add to the image (build/root stays empty), it installs into the sysroot.
install: $(LIBS)
	@mkdir -p $(SYSROOT)/usr/include/c++ $(SYSROOT)/usr/lib build/root
	rm -rf $(SYSROOT)/usr/include/c++/v1 && cp -R $(HDRS) $(SYSROOT)/usr/include/c++/v1
	@for l in $(LIBS); do cmp -s $$l $(SYSROOT)/usr/lib/`basename $$l` || cp $$l $(SYSROOT)/usr/lib/; done
	@echo "libcxx: installed into $(SYSROOT)"

clean:
	rm -rf build
