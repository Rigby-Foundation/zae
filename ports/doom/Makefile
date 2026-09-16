# SPDX-License-Identifier: GPL-2.0-only
# Doom port for sic OS using doomgeneric (GPL-2.0). The game data is
# Freedoom Phase 1 (BSD-3-Clause, freely redistributable) - id's doom1.wad
# is shareware and can't be bundled. `make DOOM_WAD=none` ships the engine
# alone; then put any IWAD in /usr/share/games/doom or the working directory
# (e.g. on /disk) and run `doom -iwad /disk/doom.wad`.

DOOMGENERIC_URL := https://github.com/ozkl/doomgeneric/archive/refs/heads/master.tar.gz
FREEDOOM_VER := 0.13.0
WAD_URL := https://github.com/freedoom/freedoom/releases/download/v$(FREEDOOM_VER)/freedoom-$(FREEDOOM_VER).zip
DOOM_WAD ?= freedoom1.wad

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
LLD_PREFIX  ?= $(firstword $(foreach f,lld lld@21 lld@20 llvm,$(if $(wildcard $(shell brew --prefix $(f) 2>/dev/null)/bin/ld.lld),$(shell brew --prefix $(f)),)))
ifeq ($(origin CC),default)
CC := $(LLVM_PREFIX)/bin/clang
endif
ifeq ($(origin LD),default)
LD := $(LLD_PREFIX)/bin/ld.lld
endif

SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)
LIBC    := $(SYSROOT)/usr
SRC     := src
BUILD   := build
ROOT    := $(BUILD)/root

CFLAGS  := --target=x86_64-linux-musl -std=gnu11 -nostdinc -isystem $(LIBC)/include \
           -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -g0 \
           -Wno-everything -D_GNU_SOURCE -DNORMALUNIX -DLINUX -DSNDSERV -D_DEFAULT_SOURCE \
           -I$(SRC) -DFILES_DIR='"/usr/share/games/doom"'
LDFLAGS := -static -nostdlib --image-base=0x8000000000 -z max-page-size=0x1000 -z noexecstack \
           --defsym=_DYNAMIC=0x8000000000
CRT_BEGIN := $(LIBC)/lib/crt1.o $(LIBC)/lib/crti.o
CRT_END   := $(LIBC)/lib/crtn.o
OSABI_SIC := \123

CORE_SRCS := dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c d_iwad.c \
             d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c hu_lib.c hu_stuff.c \
             info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c i_sound.c i_system.c i_timer.c \
             memio.c m_argv.c m_bbox.c m_cheat.c m_config.c m_controls.c m_fixed.c m_menu.c \
             m_misc.c m_random.c p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c \
             p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c \
             p_spec.c p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c r_data.c r_draw.c \
             r_main.c r_plane.c r_segs.c r_sky.c r_things.c sha1.c sounds.c statdump.c st_lib.c \
             st_stuff.c s_sound.c tables.c v_video.c wi_stuff.c w_checksum.c w_file.c w_main.c \
             w_wad.c z_zone.c w_file_stdc.c i_input.c i_video.c doomgeneric.c

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRCS)) $(BUILD)/doomgeneric_sic.o

.PHONY: all fetch clean

ifeq ($(DOOM_WAD),none)
all: $(ROOT)/bin/doom
else
all: $(ROOT)/bin/doom $(ROOT)/usr/share/games/doom/$(DOOM_WAD)
endif

fetch: $(SRC)/doomgeneric.c $(BUILD)/freedoom1.wad

$(SRC)/doomgeneric.c:
	@mkdir -p $(SRC)
	curl -sSL $(DOOMGENERIC_URL) | tar -xzf - --strip-components=2 -C $(SRC) "doomgeneric-master/doomgeneric"

$(BUILD)/freedoom1.wad:
	@mkdir -p $(BUILD)
	curl -sSL -o $(BUILD)/freedoom.zip $(WAD_URL)
	unzip -p $(BUILD)/freedoom.zip freedoom-$(FREEDOOM_VER)/freedoom1.wad > $@
	unzip -p $(BUILD)/freedoom.zip freedoom-$(FREEDOOM_VER)/COPYING.txt > $(BUILD)/COPYING.freedoom
	rm -f $(BUILD)/freedoom.zip

$(BUILD)/%.o: $(SRC)/%.c $(SRC)/doomgeneric.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/doomgeneric_sic.o: doomgeneric_sic.c $(SRC)/doomgeneric.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ROOT)/bin/doom: $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(CRT_BEGIN) $(OBJS) $(LIBC)/lib/libc.a $(CRT_END)
	@printf '$(OSABI_SIC)' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none

$(ROOT)/usr/share/games/doom/freedoom1.wad: $(BUILD)/freedoom1.wad
	@mkdir -p $(dir $@)
	cp $< $@
	cp $(BUILD)/COPYING.freedoom $(dir $@)/COPYING.freedoom

clean:
	rm -rf $(BUILD)
