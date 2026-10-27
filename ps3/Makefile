# Compile check of the game sources for the PS3: LP64, big-endian, ELFv1, no PSL1GHT code yet.
# ./build.sh -j12 check, then ./build.sh report and ./build.sh toc. Each object keeps its diagnostics in <obj>.log.
# TOOLCHAIN=linux (build_linux.sh): GCC 13 for ppc64 big-endian ELFv1, for the sources ppu-gcc 7.2 rejects.
# It uses the PS3 code model (small, FP constants in the TOC), so its TOC numbers approximate ppu-gcc's.
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

.PHONY: check report toc dvdtest audiotest gxtest scenetest progtest gxeboot spu clean
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
TEST_DEPS := $(ROOT)/ps3/dvd/dvd.c $(ROOT)/ps3/os/intr.c $(ROOT)/$(SYNC_SRC)
TEST_HEADERS := $(ROOT)/ps3/dvd/dvd.h $(ROOT)/ps3/os/intr.h
dvdtest: $(BUILD)/dvd_test $(BUILD)/intr_test
$(BUILD)/%_test: $(ROOT)/ps3/tests/%_test.c $(TEST_DEPS) $(TEST_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DPS3_APP_DIR=\"$(DVD_APP_DIR)\" -static -o $@ $< $(TEST_DEPS) -lpthread

# The SDK's AX library over ps3/audio, with the audio program on a host thread (TOOLCHAIN=linux).
AUDIO_TEST_SRCS := $(addprefix $(ROOT)/ps3/,tests/audio_test.c tests/audio_host.c audio/ax_spu.c \
                   audio/sdk_audio.c os/intr.c os/sync_host.c os/globals.c) $(addprefix $(ROOT)/,$(SDK_AUDIO_SRCS))
audiotest: $(BUILD)/audio_test
$(BUILD)/audio_test: $(AUDIO_TEST_SRCS) $(wildcard $(ROOT)/ps3/audio/*.h $(ROOT)/ps3/tests/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(AUDIO_TEST_SRCS) -lpthread -lm

# The SDK's GX library over the GP thread, with a backend that records what it gets (TOOLCHAIN=linux).
GX_TEST_SRCS := $(addprefix $(ROOT)/,ps3/tests/gx_test.c ps3/os/intr.c ps3/os/sync_host.c ps3/os/globals.c \
                ps3/tests/udplog_host.c $(GX_PORT_SRCS) \
                $(SDK_GX_SRCS))
gxtest: $(BUILD)/gx_test
$(BUILD)/gx_test: $(GX_TEST_SRCS) $(wildcard $(ROOT)/ps3/gx/*.h $(ROOT)/ps3/os/*.h $(ROOT)/ps3/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(GX_TEST_SRCS) -lpthread -lm

# The RSX backend over a software RSX: the GX test frame as PPM files (TOOLCHAIN=linux, big-endian hosts).
GX_RSX_SRCS := $(addprefix ps3/gx/,fpgen.c tevgen.c vpgen.c xfgen.c vtx.c texdec.c backend.c)
SCENE_TEST_SRCS := $(addprefix $(ROOT)/,ps3/tests/gx_scene_test.c ps3/tests/gx_scene.c ps3/tests/gx_stubs.c \
                   ps3/tests/rsxdev_soft.c ps3/tests/rsx_interp.c ps3/tests/udplog_host.c ps3/os/intr.c \
                   ps3/os/sync_host.c ps3/os/globals.c $(GX_PORT_SRCS) \
                   $(GX_RSX_SRCS) $(SDK_GX_SRCS))
scenetest: $(BUILD)/gx_scene_test
$(BUILD)/gx_scene_test: $(SCENE_TEST_SRCS) $(wildcard $(ROOT)/ps3/gx/*.h $(ROOT)/ps3/os/*.h $(ROOT)/ps3/tests/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -static -o $@ $(SCENE_TEST_SRCS) -lpthread -lm

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
spu: $(BUILD)/spu/ax_spu.elf
$(BUILD)/spu/ax_spu.elf: $(SPU_SRCS) $(wildcard $(ROOT)/ps3/audio/*.h)
	@mkdir -p $(dir $@)
	$(SPU_CC) $(SPU_CFLAGS) -Wl,--gc-sections -s $(SPU_SRCS) -L$(PS3DEV)/spu/lib -lsputhread -o $@
	@$(PS3DEV)/spu/bin/spu-size $@

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
