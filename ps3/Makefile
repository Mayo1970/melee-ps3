# The PS3 port (ppu-gcc, ./build.sh) and its host builds and tests (TOOLCHAIN=linux, GCC 13 ppc64 big-endian ELFv1
# with the PS3 code model, ./build_linux.sh). Each object keeps its diagnostics in <obj>.log.
TOOLCHAIN ?= ppu
ROOT := ..

ifeq ($(TOOLCHAIN),linux)
PREFIX := powerpc64-linux-gnu-
# newlib's <sys/types.h> brings in uintptr_t; glibc's does not. glibc's math.h declares __fabs, which
# placeholder.h defines as a macro, so math.h goes first.
ARCH   := -mcpu=cell -mcmodel=small -mfp-in-toc -include stdint.h -include math.h
BUILD  ?= build/linux
else
ifeq ($(strip $(PS3DEV)),)
$(error "PS3DEV is not set")
endif
PREFIX := $(PS3DEV)/ppu/bin/ppu-
ARCH   := -mcpu=cell -mhard-float
BUILD  ?= build/ppu
# PSL1GHT's headers, for the port's own sources only; its pthread.h comes before newlib's.
PORT_INCS := -I$(PS3DEV)/ppu/include
endif
CC := $(PREFIX)gcc

# The SDK's AX library as decompiled; the audio program on an SPU replaces the DSP it drives.
SDK_AUDIO_SRCS := $(addprefix libs/dolphin/src/dolphin/ax/,AX.c AXAlloc.c AXAux.c AXCL.c AXOut.c AXSPB.c \
                  AXVPB.c AXProf.c)
# The SDK's GX library as decompiled; the GP thread in ps3/gx stands in for the flipper's GP.
SDK_GX_SRCS := $(addprefix libs/dolphin/src/dolphin/gx/,GXAttr.c GXBump.c GXDisplayList.c GXDraw.c GXFifo.c \
               GXFrameBuf.c GXGeometry.c GXInit.c GXLight.c GXMisc.c GXPerf.c GXPixel.c GXStubs.c GXTev.c \
               GXTexture.c GXTransform.c)
GX_PORT_SRCS := ps3/gx/fifo.c ps3/gx/gp.c ps3/os/mem.c ps3/os/ppcarch.c
# Mutexes, conditions and threads: lv2 objects on the PS3 (PTE waits spin there), pthreads on the host.
ifeq ($(TOOLCHAIN),linux)
SYNC_SRC := ps3/os/sync_host.c
else
SYNC_SRC := ps3/os/sync_cell.c
endif
PORT_SRCS := ps3/dvd/dvd.c ps3/os/intr.c $(SYNC_SRC) ps3/os/globals.c ps3/audio/sdk_audio.c $(SDK_AUDIO_SRCS) \
             $(SDK_GX_SRCS) $(GX_PORT_SRCS)
ifneq ($(TOOLCHAIN),linux)
PORT_SRCS += ps3/audio/audio_cell.c
endif
SRCS := $(sort $(shell cd $(ROOT) && find src/melee src/sysdolphin -name '*.c')) $(PORT_SRCS)
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))

# TARGET_PC: archives go through the native .dat readers, as in the PC build. TARGET_PS3: what differs from
# the PC build, as the PS3 address map. No MUST_MATCH.
CFLAGS := -std=gnu11 -O2 $(ARCH) -ffunction-sections -fdata-sections -fno-strict-aliasing \
          -Wall -Wno-unused -Wno-missing-braces -Wno-parentheses -Wno-maybe-uninitialized \
          -DTARGET_PC -DTARGET_PS3 -Iinclude -I$(ROOT)/src -I$(ROOT)/libs/doldecomp/include -I$(ROOT)/libs/dolphin/include \
          $(EXTRA_CFLAGS)

.PHONY: check report toc dvdtest alarmtest audiotest reverbtest gxtest streamtest tevfusetest scenetest progtest thptest \
        dattest mathobjs mathtest gxeboot spu game hostgame clean
check: $(OBJS)

$(BUILD)/ps3/%.o: CFLAGS += $(PORT_INCS)

# A failed source leaves its log and a .failed marker; the others still build.
$(BUILD)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@rm -f $@.failed
	@$(CC) $(CFLAGS) -c $< -o $@ 2> $@.log || touch $@.failed

# Host tests for qemu-ppc64 (TOOLCHAIN=linux): dvd_test and intr_test. DVD_APP_DIR stands in for
# PS3_APP_DIR.
DVD_APP_DIR ?= /scratch
TEST_DEPS := $(ROOT)/ps3/dvd/dvd.c $(ROOT)/ps3/os/intr.c $(ROOT)/ps3/os/mem.c $(ROOT)/$(SYNC_SRC)
TEST_HEADERS := $(ROOT)/ps3/dvd/dvd.h $(ROOT)/ps3/os/intr.h
dvdtest: $(BUILD)/dvd_test $(BUILD)/intr_test
$(BUILD)/%_test: $(ROOT)/ps3/tests/%_test.c $(TEST_DEPS) $(TEST_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DPS3_APP_DIR=\"$(DVD_APP_DIR)\" -static -o $@ $< $(TEST_DEPS) -lpthread

# The pad wait's sleep (ps3_intr_wait_alarms) under OS alarms (TOOLCHAIN=linux).
ALARM_TEST_SRCS := $(addprefix $(ROOT)/ps3/,tests/alarm_test.c os/intr.c os/alarm.c os/ostime.c os/globals.c os/sync_host.c)
alarmtest: $(BUILD)/alarm_test
$(BUILD)/alarm_test: $(ALARM_TEST_SRCS) $(wildcard $(ROOT)/ps3/os/*.h) $(ROOT)/ps3/include/ps3_intr.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(ALARM_TEST_SRCS) -lpthread

# The SDK's AX library over ps3/audio, with the audio program on a host thread (TOOLCHAIN=linux).
AUDIO_TEST_SRCS := $(addprefix $(ROOT)/ps3/,tests/audio_test.c tests/audio_host.c audio/ax_spu.c \
                   audio/sdk_audio.c os/intr.c os/mem.c os/sync_host.c os/globals.c) $(addprefix $(ROOT)/,$(SDK_AUDIO_SRCS)) \
                   $(addprefix $(ROOT)/libs/dolphin/src/dolphin/axfx/,axfx.c chorus.c delay.c reverb_hi.c reverb_std.c)
audiotest: $(BUILD)/audio_test
$(BUILD)/audio_test: $(AUDIO_TEST_SRCS) $(wildcard $(ROOT)/ps3/audio/*.h $(ROOT)/ps3/tests/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(AUDIO_TEST_SRCS) -lpthread -lm

# reverb_std.c's HandleReverb against the version it replaced, bit for bit (TOOLCHAIN=linux).
reverbtest: $(BUILD)/reverb_test
$(BUILD)/reverb_test: $(ROOT)/ps3/tests/reverb_test.c $(ROOT)/libs/dolphin/src/dolphin/axfx/reverb_std.c \
                      $(ROOT)/libs/dolphin/include/dolphin/axfx.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $< -lm

# The SDK's GX library over the GP thread, with a backend that records what it gets (TOOLCHAIN=linux).
GX_TEST_SRCS := $(addprefix $(ROOT)/,ps3/tests/gx_test.c ps3/os/intr.c ps3/os/sync_host.c ps3/os/globals.c \
                ps3/tests/udplog_host.c $(GX_PORT_SRCS) \
                $(SDK_GX_SRCS))
gxtest: $(BUILD)/gx_test
$(BUILD)/gx_test: $(GX_TEST_SRCS) $(wildcard $(ROOT)/ps3/gx/*.h $(ROOT)/ps3/os/*.h $(ROOT)/ps3/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(GX_TEST_SRCS) -lpthread -lm

GX_SDK_HEADERS := $(ROOT)/libs/dolphin/src/dolphin/gx/__gx.h $(wildcard $(ROOT)/libs/dolphin/include/dolphin/gx/*.h)
# Every SDK GX call that writes the pipe, with random arguments; -DGX_PIPE_HASH hashes the stream (TOOLCHAIN=linux).
STREAM_TEST_SRCS := $(addprefix $(ROOT)/,ps3/tests/gx_stream_test.c ps3/os/intr.c ps3/os/sync_host.c ps3/os/globals.c                     ps3/tests/udplog_host.c ps3/gx/fifo.c ps3/os/mem.c ps3/os/ppcarch.c $(SDK_GX_SRCS))
streamtest: $(BUILD)/gx_stream_test
$(BUILD)/gx_stream_test: $(STREAM_TEST_SRCS) $(wildcard $(ROOT)/ps3/gx/*.h $(ROOT)/ps3/os/*.h $(ROOT)/ps3/include/*.h) \
                         $(GX_SDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(STREAM_TEST_SRCS) -lpthread -lm

# GXSetTevStagePS3 against the seven TEV stage calls it replaces (TOOLCHAIN=linux).
TEVFUSE_TEST_SRCS := $(addprefix $(ROOT)/,ps3/tests/gx_tevfuse_test.c libs/dolphin/src/dolphin/gx/GXTev.c)
tevfusetest: $(BUILD)/gx_tevfuse_test
$(BUILD)/gx_tevfuse_test: $(TEVFUSE_TEST_SRCS) $(GX_SDK_HEADERS) $(ROOT)/ps3/include/gx_pipe.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(TEVFUSE_TEST_SRCS) -lm

# The RSX backend over a software RSX: the GX test frame as PPM files (TOOLCHAIN=linux, big-endian hosts).
GX_RSX_SRCS := $(addprefix ps3/gx/,fpgen.c tevgen.c vpgen.c xfgen.c vtx.c texdec.c backend.c vxjob.c)
SCENE_TEST_SRCS := $(addprefix $(ROOT)/,ps3/tests/gx_scene_test.c ps3/tests/gx_scene.c ps3/tests/gx_stubs.c \
                   ps3/tests/rsxdev_soft.c ps3/tests/rsx_interp.c ps3/tests/udplog_host.c ps3/os/intr.c \
                   ps3/os/sync_host.c ps3/os/globals.c ps3/tests/vxspu_host.c ps3/gx/vxspu.c $(GX_PORT_SRCS) \
                   $(GX_RSX_SRCS) $(SDK_GX_SRCS))
scenetest: $(BUILD)/gx_scene_test
$(BUILD)/gx_scene_test: $(SCENE_TEST_SRCS) $(wildcard $(ROOT)/ps3/gx/*.h $(ROOT)/ps3/os/*.h $(ROOT)/ps3/tests/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(SCENE_TEST_SRCS) -lpthread -lm

# The THP decoder over the game's movies (TOOLCHAIN=linux): frames as PPM files and as standard JPEG files.
THP_TEST_SRCS := $(addprefix $(ROOT)/,ps3/tests/thp_test.c ps3/sdk/thp.c)
thptest: $(BUILD)/thp_test
$(BUILD)/thp_test: $(THP_TEST_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(THP_TEST_SRCS)

# The generated programs against CPU references, on the ucode interpreters (any host).
PROG_TEST_SRCS := $(addprefix $(ROOT)/,ps3/tests/prog_test.c ps3/tests/rsx_interp.c ps3/gx/fpgen.c ps3/gx/tevgen.c \
                  ps3/gx/vpgen.c ps3/gx/xfgen.c)
progtest: $(BUILD)/prog_test
$(BUILD)/prog_test: $(PROG_TEST_SRCS) $(wildcard $(ROOT)/ps3/gx/*.h $(ROOT)/ps3/tests/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(PROG_TEST_SRCS) -lm

# The GX test EBOOT (ppu toolchain): the GX test frame on the RSX. The title ID is the placeholder of ps3/dvd.
EBOOT_TITLE := Melee GX test
EBOOT_APPID := MELE00001
EBOOT_CONTENTID := UP0001-$(EBOOT_APPID)_00-0000000000000000
EBOOT_DIR := $(BUILD)/gxeboot
GX_EBOOT_SRCS := ps3/gx_eboot.c ps3/tests/gx_scene_run.c ps3/tests/gx_scene.c ps3/tests/gx_stubs.c \
                 ps3/gx/rsxdev_cell.c ps3/os/udplog_cell.c ps3/os/intr.c ps3/os/sync_cell.c ps3/os/globals.c \
                 $(GX_PORT_SRCS) $(GX_RSX_SRCS) $(SDK_GX_SRCS)
GX_EBOOT_OBJS := $(patsubst %.c,$(EBOOT_DIR)/obj/%.o,$(GX_EBOOT_SRCS))
# The test reports through the UDP log, so it keeps it in any build.
$(EBOOT_DIR)/obj/ps3/os/udplog_cell.o: CFLAGS += -DPS3_UDPLOG
EBOOT_LIBS := -L$(PS3DEV)/ppu/lib -lrsx -lgcm_sys -lsysutil -lsysmodule -lnet -lpthread -llv2 -lrt -lm

$(EBOOT_DIR)/obj/%.o: $(ROOT)/%.c $(wildcard $(ROOT)/ps3/gx/*.h $(ROOT)/ps3/os/*.h $(ROOT)/ps3/tests/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PORT_INCS) -c $< -o $@

gxeboot: $(EBOOT_DIR)/gx_test.pkg
$(EBOOT_DIR)/gx_test.elf: $(GX_EBOOT_OBJS)
	$(CC) $(ARCH) -Wl,--no-multi-toc -Wl,--gc-sections $(GX_EBOOT_OBJS) $(EBOOT_LIBS) -o $@
	$(PREFIX)nm $@ | grep -c r2off || true
	$(PREFIX)readelf -SW $@ | grep -E ' \.(got|toc|text|data|bss) '

$(EBOOT_DIR)/gx_test.pkg: $(EBOOT_DIR)/gx_test.elf
	$(PREFIX)strip $< -o $(EBOOT_DIR)/gx_test.strip.elf
	sprxlinker $(EBOOT_DIR)/gx_test.strip.elf
	fself $(EBOOT_DIR)/gx_test.strip.elf $(EBOOT_DIR)/EBOOT.BIN
	@mkdir -p $(EBOOT_DIR)/pkg/USRDIR
	cp $(PS3DEV)/bin/ICON0.PNG $(EBOOT_DIR)/pkg/ICON0.PNG
	make_self_npdrm $(EBOOT_DIR)/gx_test.strip.elf $(EBOOT_DIR)/pkg/USRDIR/EBOOT.BIN $(EBOOT_CONTENTID) > /dev/null
	sfo --title "$(EBOOT_TITLE)" --appid "$(EBOOT_APPID)" -f $(PS3DEV)/bin/sfo.xml $(EBOOT_DIR)/pkg/PARAM.SFO
	pkg --contentid $(EBOOT_CONTENTID) $(EBOOT_DIR)/pkg/ $@ > /dev/null
	@ls -l $(EBOOT_DIR)/EBOOT.BIN $@

# The audio program for the SPU, with the same sources the host test runs.
SPU_CC := $(PS3DEV)/spu/bin/spu-gcc
SPU_CFLAGS := -O2 -Wall -ffunction-sections -fdata-sections -I$(PS3DEV)/spu/include
SPU_SRCS := $(ROOT)/ps3/audio/ax_spu.c $(ROOT)/ps3/audio/spu_io_cell.c
spu: $(BUILD)/spu/ax_spu.elf $(BUILD)/spu/vx_spu.elf $(BUILD)/spu/rb_spu.elf
$(BUILD)/spu/ax_spu.elf: $(SPU_SRCS) $(wildcard $(ROOT)/ps3/audio/*.h)
	@mkdir -p $(dir $@)
	$(SPU_CC) $(SPU_CFLAGS) -Wl,--gc-sections -s $(SPU_SRCS) -L$(PS3DEV)/spu/lib -lsputhread -o $@
	@$(PS3DEV)/spu/bin/spu-size $@
# The vertex program for the SPU (gx/vxjob.h), with the same source the host builds run on a thread.
VX_SPU_SRCS := $(ROOT)/ps3/gx/vxspu.c $(ROOT)/ps3/gx/vxspu_cell.c
$(BUILD)/spu/vx_spu.elf: $(VX_SPU_SRCS) $(ROOT)/ps3/gx/vx_shared.h $(ROOT)/ps3/gx/vtx_step.h $(ROOT)/ps3/gx/vxspu_io.h
	@mkdir -p $(dir $@)
	$(SPU_CC) $(SPU_CFLAGS) -Wl,--gc-sections -s $(VX_SPU_SRCS) -L$(PS3DEV)/spu/lib -lsputhread -o $@
	@$(PS3DEV)/spu/bin/spu-size $@
# The rollback's program for the SPU (net/rb_shared.h), with the same source the host builds run on a thread.
RB_SPU_SRCS := $(ROOT)/ps3/net/rbspu.c $(ROOT)/ps3/net/rbspu_cell.c
$(BUILD)/spu/rb_spu.elf: $(RB_SPU_SRCS) $(ROOT)/ps3/net/rb_shared.h $(ROOT)/ps3/net/rbspu_io.h
	@mkdir -p $(dir $@)
	$(SPU_CC) $(SPU_CFLAGS) -Wl,--gc-sections -s $(RB_SPU_SRCS) -L$(PS3DEV)/spu/lib -lsputhread -o $@
	@$(PS3DEV)/spu/bin/spu-size $@

# Melee as an EBOOT (ppu toolchain): the game, the SDK libraries the port builds, the port, the SPU program and
# the native .dat readers over the schema from tools/datgen. Game and schema objects get -mminimal-toc.
GAME_TITLE := Super Smash Bros: Melee
GAME_DIR := $(BUILD)/game
DOL := $(ROOT)/orig/GALE01/sys/main.dol
DAT_SCHEMA := ps3/build/datgen/schema
GAME_SRCS := $(sort $(shell cd $(ROOT) && find src/melee src/sysdolphin -name '*.c')) src/MSL/float.c
GAME_SDK_SRCS := $(SDK_AUDIO_SRCS) $(SDK_GX_SRCS) libs/dolphin/src/dolphin/os/OSArena.c \
                 libs/dolphin/src/dolphin/mtx/mtx44.c libs/dolphin/src/dolphin/pad/PadClamp.c \
                 $(addprefix libs/dolphin/src/dolphin/axfx/,axfx.c chorus.c delay.c reverb_hi.c reverb_std.c)
GAME_PORT_SRCS := $(addprefix ps3/,dvd/dvd.c os/intr.c os/globals.c os/os.c os/ostime.c os/heap.c \
                  os/alarm.c os/thread.c os/mem.c os/ppcarch.c audio/sdk_audio.c gx/fifo.c gx/gp.c sdk/mtx.c \
                  sdk/trig.c sdk/card.c sdk/stubs.c sdk/thp.c pad/pad.c pad/menu.c vi/vi.c dat/pool.c dat/trace.c \
                  os/calltrace.c \
                  os/mainprof.c net/rollback.c net/netplay.c net/upnp.c) $(GX_RSX_SRCS) \
                  $(EXTRA_SRCS)
# The PSL1GHT side, which no SDK header meets.
GAME_CELL_SRCS := $(addprefix ps3/,os/sync_cell.c os/sys_cell.c os/udplog_cell.c audio/audio_cell.c \
                  gx/rsxdev_cell.c gx/vxjob_cell.c pad/pad_cell.c pad/gcadapter_cell.c net/rbjob_cell.c \
                  net/link_cell.c)
DAT_SRCS := $(addprefix tools/dat-cli/native/src/,archive.c reader.c hsd.c) ps3/dat/lookup.c ps3/dat/motion.c \
            $(sort $(shell cd $(ROOT) && find $(DAT_SCHEMA) -name '*.c' 2> /dev/null))
# Their allocations come from the .dat pool; GeoTypes.h stands in for Aurora's.
DAT_FLAGS := -include $(ROOT)/ps3/dat/redirect.h -I$(ROOT)/tools/dat-cli/native/include -I$(ROOT)/$(DAT_SCHEMA) \
             -I$(ROOT)/ps3/dat/include -DVERSION_GALE01 -DBUILD_VERSION=0
# The host game build (TOOLCHAIN=linux) replaces the PSL1GHT side: the audio program on a thread, the software RSX
# without drawing, no pads or a script, the lv2 calls on Linux. glibc's setjmp keeps 64-bit registers.
ifeq ($(TOOLCHAIN),linux)
GAME_CELL_SRCS := $(addprefix ps3/,os/sync_host.c tests/sys_host.c tests/udplog_host.c tests/audio_host.c \
                  audio/ax_spu.c tests/rsxdev_soft.c tests/rsx_interp.c tests/pad_host.c tests/host_start.c \
                  tests/vxspu_host.c gx/vxspu.c tests/rbspu_host.c net/rbspu.c tests/link_host.c)
HOST_WRAP := -Wl,--wrap=gm_ChangeGameModeAfterCurrentScene,--wrap=gm_LoadAnnouncer,--wrap=gm_SetGameModeStateId
endif
GAME_OBJ = $(patsubst %.c,$(GAME_DIR)/%.o,$(1))
GAME_OBJS := $(call GAME_OBJ,$(GAME_SRCS) $(GAME_SDK_SRCS) $(GAME_PORT_SRCS) $(GAME_CELL_SRCS) $(DAT_SRCS))
ifneq ($(TOOLCHAIN),linux)
GAME_OBJS += $(GAME_DIR)/ps3/os/setjmp.o $(GAME_DIR)/ax_spu_elf.o $(GAME_DIR)/vx_spu_elf.o $(GAME_DIR)/rb_spu_elf.o
endif
# Untyped lookups become typed ones, parses free the native data of the buffers they reuse (ps3/dat/lookup.c);
# the trace logs typed lookups (step 3). The scene loop's logic, render and XFB calls are timed (os/mainprof.c).
GAME_WRAP := -Wl,--wrap=HSD_ArchiveParse,--wrap=HSD_ArchiveGetPublicAddress,--wrap=HSD_ArchiveGetPublicTyped \
             -Wl,--wrap=HSD_ArchiveReleaseNative \
             -Wl,--wrap=HSD_GObj_RunProcs,--wrap=HSD_GObj_80390FC0,--wrap=HSD_VICopyXFBAsync
GAME_LIBS := -L$(PS3DEV)/ppu/lib -lrsx -lgcm_sys -lio -lusb -laudio -lsysutil -lsysmodule -lnetctl -lnet -llv2 -lrt -lm
FONT_INCS := $(addprefix $(GAME_DIR)/include/sysdolphin/baselib/,debug_font.inc sislib_font.inc)

# MSL's math where it differs from libm; string literals in Shift-JIS, as sjiswrap gives the MWCC build (CP932
# keeps the backslash).
$(GAME_DIR)/src/%.o: GAME_FLAGS := -mminimal-toc -include ps3_msl_math.h -finput-charset=UTF-8 -fexec-charset=CP932
$(call GAME_OBJ,$(GAME_CELL_SRCS)): GAME_FLAGS := $(PORT_INCS)
$(call GAME_OBJ,$(DAT_SRCS)): GAME_FLAGS := -mminimal-toc $(DAT_FLAGS)
# The only fused operations of newlib's sinef replica are its own fmas.
$(call GAME_OBJ,ps3/sdk/trig.c): GAME_FLAGS := -ffp-contract=off
ifeq ($(TOOLCHAIN),linux)
$(call GAME_OBJ,ps3/dvd/dvd.c): GAME_FLAGS := -DPS3_APP_DIR=\"$(DVD_APP_DIR)\"
# The disc folder is read-only there: the memory card lives in the build tree.
$(call GAME_OBJ,ps3/sdk/card.c): GAME_FLAGS := -DPS3_CARD_DIR=\"build/card\"
$(call GAME_OBJ,ps3/pad/menu.c): GAME_FLAGS := -DPS3_OPTIONS_PATH=\"build/options.txt\"
$(call GAME_OBJ,ps3/tests/audio_host.c): GAME_FLAGS := -DHOST_AUDIO_REALTIME
endif

# -fno-common: uninitialized globals get their own .bss sections too, which melee.ld aligns to 32 bytes.
$(GAME_DIR)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -fno-common -I$(GAME_DIR)/include $(GAME_FLAGS) -MMD -MP -c $< -o $@ 2> $@.log || \
	    (cat $@.log; false)

$(GAME_DIR)/ps3/os/setjmp.o: $(ROOT)/ps3/os/setjmp.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -c $< -o $@

$(GAME_DIR)/ax_spu_elf.o: $(BUILD)/spu/ax_spu.elf
	@mkdir -p $(GAME_DIR)
	cd $(dir $<) && bin2s -a 128 ax_spu.elf > $(CURDIR)/$(GAME_DIR)/ax_spu_elf.S
	$(CC) $(ARCH) -x assembler-with-cpp -c $(GAME_DIR)/ax_spu_elf.S -o $@
$(GAME_DIR)/vx_spu_elf.o: $(BUILD)/spu/vx_spu.elf
	@mkdir -p $(GAME_DIR)
	cd $(dir $<) && bin2s -a 128 vx_spu.elf > $(CURDIR)/$(GAME_DIR)/vx_spu_elf.S
	$(CC) $(ARCH) -x assembler-with-cpp -c $(GAME_DIR)/vx_spu_elf.S -o $@
$(GAME_DIR)/rb_spu_elf.o: $(BUILD)/spu/rb_spu.elf
	@mkdir -p $(GAME_DIR)
	cd $(dir $<) && bin2s -a 128 rb_spu.elf > $(CURDIR)/$(GAME_DIR)/rb_spu_elf.S
	$(CC) $(ARCH) -x assembler-with-cpp -c $(GAME_DIR)/rb_spu_elf.S -o $@

# The two fonts the game reads from its DOL's data, as the matching build extracts them (config.yml).
$(GAME_DIR)/include/sysdolphin/baselib/debug_font.inc: $(DOL)
	@mkdir -p $(dir $@)
	python3 tools/dol_inc.py $< 0x804088B8 0x1C00 $@
$(GAME_DIR)/include/sysdolphin/baselib/sislib_font.inc: $(DOL)
	@mkdir -p $(dir $@)
	python3 tools/dol_inc.py $< 0x8040CD40 0x23E00 $@
$(call GAME_OBJ,src/sysdolphin/baselib/hsd_3915.c src/sysdolphin/baselib/sislib_font.c): $(FONT_INCS)

# The rollback's test mode in a console build, e.g. RB_FLAGS=-DPS3_RB_SYNC=2 (ps3/net/rollback.c); a change of
# RB_FLAGS rebuilds that object only.
RB_FLAGS ?=
.PHONY: FORCE
$(GAME_DIR)/rb_flags.txt: FORCE
	@mkdir -p $(dir $@)
	@echo '$(RB_FLAGS)' | cmp -s - $@ || echo '$(RB_FLAGS)' > $@
$(call GAME_OBJ,ps3/net/rollback.c): GAME_FLAGS := $(RB_FLAGS)
$(call GAME_OBJ,ps3/net/rollback.c): $(GAME_DIR)/rb_flags.txt

# DEBUG=1: a debug build, with the UDP log to the PC (ps3/os/udplog.h); retail builds leave it out. A change of
# DEBUG rebuilds that object only.
DEBUG ?= 0
$(GAME_DIR)/debug_flags.txt: FORCE
	@mkdir -p $(dir $@)
	@echo '$(DEBUG)' | cmp -s - $@ || echo '$(DEBUG)' > $@
ifeq ($(DEBUG),1)
$(call GAME_OBJ,ps3/os/udplog_cell.c): GAME_FLAGS += -DPS3_UDPLOG
endif
$(call GAME_OBJ,ps3/os/udplog_cell.c): $(GAME_DIR)/debug_flags.txt

-include $(GAME_OBJS:.o=.d)

game: $(GAME_DIR)/melee.pkg
$(GAME_DIR)/melee.elf: $(GAME_OBJS) melee.ld rollback_data.ld rollback_bss.ld
	$(CC) $(ARCH) -T melee.ld $(GAME_WRAP) -Wl,--no-multi-toc -Wl,--gc-sections -Wl,-Map,$(GAME_DIR)/melee.map \
	    $(EXTRA_LDFLAGS) $(GAME_OBJS) $(GAME_LIBS) -o $@
	@echo "r2off stubs: $$($(PREFIX)nm $@ | grep -c r2off)"
	@$(PREFIX)readelf -SW $@ | grep -E ' \.(got|toc|text|rodata|data|bss) '
	@$(PREFIX)readelf -lW $@ | grep LOAD

$(GAME_DIR)/melee.pkg: $(GAME_DIR)/melee.elf $(ROOT)/ps3/ICON0.PNG
	$(PREFIX)strip $< -o $(GAME_DIR)/melee.strip.elf
	sprxlinker $(GAME_DIR)/melee.strip.elf
	fself $(GAME_DIR)/melee.strip.elf $(GAME_DIR)/EBOOT.BIN
	@mkdir -p $(GAME_DIR)/pkg/USRDIR
	cp $(ROOT)/ps3/ICON0.PNG $(GAME_DIR)/pkg/ICON0.PNG
	make_self_npdrm $(GAME_DIR)/melee.strip.elf $(GAME_DIR)/pkg/USRDIR/EBOOT.BIN $(EBOOT_CONTENTID) > /dev/null
	sfo --title "$(GAME_TITLE)" --appid "$(EBOOT_APPID)" -f $(PS3DEV)/bin/sfo.xml $(GAME_DIR)/pkg/PARAM.SFO
	pkg --contentid $(EBOOT_CONTENTID) $(GAME_DIR)/pkg/ $@ > /dev/null
	@ls -l $(GAME_DIR)/EBOOT.BIN $@

# Melee as a Linux program (TOOLCHAIN=linux), for qemu-ppc64 with a 4 GB guest space, as the PS3's user addresses:
# docker run -v <dir with games/Melee.iso>:/scratch ... qemu-ppc64 -R 0x100000000 build/linux/game/melee_host
hostgame: $(GAME_DIR)/melee_host

# The default script with SUBALIGN(32) on .rodata, .data and .bss, and the rollback's ranges, as melee.ld.
$(GAME_DIR)/host.ld: Makefile
	@mkdir -p $(dir $@)
	$(CC) -static -Wl,--verbose 2> /dev/null | sed -n '/^=====/,/^=====/p' | sed '1d;$$d' | \
	    sed -E 's/^  (\.rodata|\.data|\.bss)( +):/  \1\2: SUBALIGN(32)/' | \
	    sed -E 's/^( *)(\*\(\.data \.data\.\* \.gnu\.linkonce\.d\.\*\))/\1INCLUDE rollback_data.ld\n\1\2/' | \
	    sed -E 's/^( *)(\*\(\.bss \.bss\.\* \.gnu\.linkonce\.b\.\*\))/\1INCLUDE rollback_bss.ld\n\1\2/' > $@.tmp
	@test "$$(grep -c 'SUBALIGN(32)' $@.tmp)" = 3 && test "$$(grep -c 'INCLUDE rollback_' $@.tmp)" = 2 && \
	    mv $@.tmp $@

$(GAME_DIR)/melee_host: $(GAME_OBJS) $(GAME_DIR)/host.ld rollback_data.ld rollback_bss.ld
	$(CC) $(ARCH) -static -T $(GAME_DIR)/host.ld $(GAME_WRAP) $(HOST_WRAP) -Wl,--gc-sections \
	    -Wl,-Map,$(GAME_DIR)/melee_host.map $(EXTRA_LDFLAGS) $(GAME_OBJS) -lpthread -lm -o $@
	@$(PREFIX)readelf -lW $@ | grep LOAD

# The native .dat readers as the EBOOT builds them, under upstream's HSD test over the game's files
# (TOOLCHAIN=linux): docker run ... ppc64-qemu-test:2 qemu-ppc64 build/linux/dattest/dat_test ../orig/GALE01/files
DAT_TEST_DIR := $(BUILD)/dattest
DAT_TEST_SRCS := $(DAT_SRCS) tools/dat-cli/native/tests/hsd.c $(addprefix ps3/,tests/dat_test_init.c tests/gx_stubs.c \
                 tests/udplog_host.c dat/pool.c os/heap.c os/mem.c os/sync_host.c)
DAT_TEST_OBJS := $(patsubst %.c,$(DAT_TEST_DIR)/%.o,$(DAT_TEST_SRCS))
$(patsubst %.c,$(DAT_TEST_DIR)/%.o,$(DAT_SRCS)): TEST_FLAGS := $(DAT_FLAGS)
$(DAT_TEST_DIR)/tools/dat-cli/native/tests/hsd.o: TEST_FLAGS := -I$(ROOT)/tools/dat-cli/native/include \
    -I$(ROOT)/$(DAT_SCHEMA) -I$(ROOT)/ps3/dat/include -DVERSION_GALE01 -DBUILD_VERSION=0

$(DAT_TEST_DIR)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(TEST_FLAGS) -c $< -o $@ 2> $@.log || (cat $@.log; false)

dattest: $(DAT_TEST_DIR)/dat_test
$(DAT_TEST_DIR)/dat_test: $(DAT_TEST_OBJS)
	$(CC) $(CFLAGS) -static -o $@ $(DAT_TEST_OBJS) -lpthread -lm

# The math replicas against the code they replace, bit for bit (tests/math_test.c): ./build.sh mathobjs builds the
# game's objects and the references with ppu-gcc, then ./build_linux.sh mathtest links them with newlib's libm.a.
MATH_DIR := build/math
MATH_OBJS := $(addprefix $(MATH_DIR)/,trig.o sdkmtx.o hsdmtx.o fobj.o ref.o libm.a)
ifneq ($(TOOLCHAIN),linux)
mathobjs: $(MATH_OBJS)
$(MATH_DIR)/trig.o: $(GAME_DIR)/ps3/sdk/trig.o
$(MATH_DIR)/sdkmtx.o: $(GAME_DIR)/ps3/sdk/mtx.o
$(MATH_DIR)/hsdmtx.o: $(GAME_DIR)/src/sysdolphin/baselib/mtx.o
$(MATH_DIR)/libm.a: $(PS3DEV)/ppu/powerpc64-ps3-elf/lib/libm.a
$(MATH_DIR)/trig.o $(MATH_DIR)/sdkmtx.o $(MATH_DIR)/hsdmtx.o $(MATH_DIR)/libm.a:
	@mkdir -p $(MATH_DIR)
	cp $< $@
$(MATH_DIR)/fobj.o: $(ROOT)/ps3/tests/math_fobj.c $(ROOT)/src/sysdolphin/baselib/fobj.c \
                    $(ROOT)/src/sysdolphin/baselib/fobj.h
	@mkdir -p $(MATH_DIR)
	$(CC) $(CFLAGS) -fno-common -mminimal-toc -include ps3_msl_math.h -c $< -o $@
$(MATH_DIR)/ref.o: $(ROOT)/ps3/tests/math_ref.c
	@mkdir -p $(MATH_DIR)
	$(CC) $(CFLAGS) -fno-common -c $< -o $@
endif
# docker run ... ppc64-qemu-test:2 qemu-ppc64 build/linux/math_test trig 0 100000000 | mtx 1000000 1 | fobj
mathtest: $(BUILD)/math_test
$(BUILD)/math_test: $(ROOT)/ps3/tests/math_test.c $(MATH_OBJS)
	$(CC) $(CFLAGS) -fno-builtin -static -o $@ $< $(MATH_OBJS)

report:
	@sh tools/report.sh $(BUILD)

# Links the objects that built, with undefined symbols ignored, to measure the TOC. Run after check.
BUILT := $(wildcard $(OBJS))
LINK  := $(CC) -nostdlib -static -Wl,-e,0 -Wl,--unresolved-symbols=ignore-all
toc:
	@$(LINK) -o $(BUILD)/toc_multi.elf $(BUILT) > $(BUILD)/toc_multi.log 2>&1; echo "multi-TOC link: exit $$?"
	@$(LINK) -Wl,--no-multi-toc -o $(BUILD)/toc_single.elf $(BUILT) > $(BUILD)/toc_single.log 2>&1; \
	    echo "single-TOC link: exit $$?"
	@sh tools/toc_report.sh $(BUILD) $(PREFIX) $(BUILT)

clean:
	@rm -rf $(BUILD)
