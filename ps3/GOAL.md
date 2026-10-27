# Melee on PS3: goal and state

This file records the PS3 port of this decomp: the active goal, what is done, and what is open. A goal is done
only when the user reports `SUCCESS` from a real PS3.

## Rules

- Branch `ps3-port`. Port code lives in `ps3/`. Game and SDK sources change only where the port needs it,
  under `TARGET_PC` or `TARGET_PS3`, so the matching MWCC build keeps its code.
- Toolchain: Docker image `ps3dev-latest:latest`, PSL1GHT, `ppu-gcc` 7.2, LP64, `-O2 -mcpu=cell`.
  `ps3/build.sh` runs `make` in it. `ps3/build_linux.sh` runs the same Makefile with GCC 13 for ppc64
  big-endian ELFv1 (`ppc64-qemu-test:2`), for host tests under `qemu-ppc64`.
- Link with `-Wl,--no-multi-toc` and `-mminimal-toc` on the game objects (see Findings). Before a hardware
  test: `.got` below 0x10000, 0 `r2off` stubs, loaded segments within the RAM budget.
- Never write code against Sony's SDK. `E:\Users\Matteo\Desktop\Repo\melee-ps3` (sagemono/melee-ps3) is not
  the user's and has no license: use its findings, never its code.
- DolphinPS3 (`E:\Users\Matteo\Desktop\Repo\DolphinPS3`) is the user's: its RSX backend (`fpgen`, `TevGen`),
  pad, audio and QEMU harness may be reused. Dolphin code is GPL-2.0+.

## Active goal: Melee on the PS3

The game itself as the hardware test, with the UDP log. It replaces the separate test EBOOTs.

1. Link Melee into an EBOOT. On 2026-10-27, 234 symbols are missing (`ppu-nm` over all game, SDK and port
   objects):
   - OS (~45): threads, alarms, time, heaps and arenas, `OSInit`, reset. On the lv2 sync layer (`ps3/os/sync.h`).
   - MTX/VEC (~20): `PSMTX*`, `PSVEC*`, `MTXOrtho`, `MTXPerspective`, `MTXFrustum`, `MTXRotRad`, `C_MTXLookAt`:
     C versions of the paired-single code.
   - PAD and VI (~20): DS3 input to PAD; VI timing from the vblank, `VISetNextFrameBuffer` to `gx_rsx_present`.
   - CARD (~18): stubs that report no memory card first; saves on the HDD later.
   - AXFX and THP (~18): stubs first (no reverb, no intro video).
   - MCC, FIO, DB, cache (`DCFlushRange` and others), `PPCMfmsr`/`PPCMtmsr`: stubs or no-ops.
   - `debug.c`, `hsd_3915.c`, `sislib_font.c` (`hsd_80391*`, `HSD_Panic`, `HSD_LogInit`) and the two font atlases.
   - `ps3/dvd/dvd.c` and `ps3/audio/audio_cell.c` from pthreads to the lv2 sync layer (see Findings).
   - `.dat` loading (`HSD_ArchiveParse` and others) stubbed until step 2.
2. Native `.dat` readers: upstream's `tools/dat-cli/native` reads the archives into the game's C types for any
   byte order and pointer size. It needs the schema from `melee-dat native codegen` (DWARF build: clang, lld,
   Rust, `types.bin`) and the Aurora headers (`AURORA_SRC`). Build that toolchain, generate the schema, link the
   library for the PS3.
3. Hardware test of Melee: the first screen, with the UDP log.

Earlier hardware tests:

- GX test EBOOT (`make gxeboot`, `ps3/tests/gx_scene.c`): run 1 black screen with no response; run 2 the UDP log
  stopped when the GP thread started (PTE waits spin, see Findings); run 3 (2026-10-27) SUCCESS: all nine cells
  as expected, 60 frames per second, 33 draws per frame, 0 dropped draws, 0 unknown opcodes, nothing unsupported.
- The boot, DVD, interrupt and audio test EBOOT is dropped for step 3.

Waiting for the user:

- The title ID. The code uses the placeholder `MELE00001` (`PS3_APP_DIR` in `ps3/dvd/dvd.c`, `EBOOT_APPID` in
  `ps3/Makefile`).

The UDP log goes to `192.168.178.99`, port 18194 (`ps3/os/udplog_cell.c`). A listener: `nc -ul 18194`, or any
UDP listener on that port.

## Not done for the game

| Part | Size or note |
|---|---|
| GX on RSX | The VI layer must call `gx_rsx_present` on a frame flip (on the GP thread's side). Not drawn yet: indirect textures, TEV compare stages, two-compare alpha tests, logic ops, z textures, depth copies, emboss texgen, constant destination alpha, line width, a texcoord 7 matrix index in the vertex (the backend logs each once). Speed: a changed vertex program is uploaded again, vertices are de-indexed on the PPE for every draw (no display list cache), textures are hashed once per frame |
| OS layer | 44 OS functions: threads, alarms, time, heaps and arenas (`OSCachedToPhysical` is in `ps3/os/mem.c`) |
| PAD, VI, CARD | DS3 input, video timing, saves on the HDD |
| Native `.dat` readers | Schema generated from the PPC DWARF build (clang, lld, Rust tool, disc files) |
| Final link | Game objects, PSL1GHT libs, embedded SPU program, SELF, PKG |
| AXFX effects | `reverb_hi`, `reverb_std` and `chorus` have Gekko assembly inner loops: need C versions |

## Done

| Step | Result |
|---|---|
| Measurement (PC reference build of DolphinPS3) | Demo matches: 518 and 1,009 draws per frame; 67,124 vertices per frame (match A); 3,504 new fragment programs in 19 s with constants in the microcode; fog on 68% of draws |
| Compile check | `ppu-gcc` compiles 1,018 of 1,021 sources (game, SDK AX and GX, port). The 3 left are the ones the PC build also excludes: `debug.c`, `hsd_3915.c`, `sislib_font.c` |
| Motion flag constants | 573 `static MotionFlags const` and 15 `static u32 const` became macros; `lbfile.c` constants use a shared macro. `ppu-gcc` 7.2 rejects static const objects in constant initializers |
| TOC | 102.3 KB by default (limit 64 KB); 7.4 KB with `-mminimal-toc` on all game objects, `.text` +1.0% |
| DVD layer `ps3/dvd/` | ISO/GCM, CISO, extracted folder; disc search on HDD and USB; `disc.txt`. All 1,209 files match the PC reference from all three sources |
| Interrupt layer `ps3/os/intr.c` | One lock with per-thread state; raised interrupts run when a thread enables interrupts, or on an interrupt thread. Devcom chain test: 3,502 callbacks as interrupts, 0 inside a disabled section |
| Audio `ps3/audio/` | SPU program plays the GameCube DSP (AX microcode CRC 4e8a8b21, after Dolphin's AX HLE) and the AI (32 kHz to 48 kHz float, straight into the port ring). Host test with the real AX library: four tones on the right channel, within 0.2% of pitch, level 0.318 as expected. SPU program: 19 KB code, 21 KB data |
| ARAM, ARQ, AI, DSP shims | ARAM 16 MB, wraps at 16 MB like the hardware |
| `lbfile.c` load target | Under `TARGET_PS3`: `dst >= ARGetSize()` loads to main memory, else to ARAM |
| GX front end `ps3/gx/` | The SDK's GX library (16 sources, all Matching) builds for the PS3 and defines all 101 GX functions the game calls. Its stores go through a byte ring (`fifo.c`) to a GP thread (`gp.c`), which keeps the CP, XF and BP registers and runs display lists, draws, EFB copies, TLUT loads, tokens and draw done (PE interrupts) against a backend interface. Host test `gxtest` (qemu-ppc64): GP registers equal the SDK's shadow state; 642,248 vertices in 3,003 draws through a 64 KB ring with matching data hash, 6 of 6 runs; 100 `GXDrawDone` and one token by interrupt |
| RSX program generators `ps3/gx/` | `fpgen.c`, `tevgen.c` (from DolphinPS3, in C; TXP, EX2, RCP, fog, konst colours and TEV start values as interpolants, so the code holds no per-draw values); `vpgen.c` (NV40 vertex ucode); `xfgen.c` (XF to a vertex program: matrix-index transform, normals, GX lighting with spot and specular attenuation, texgen, post-transform matrices). Encoding checked against cgcomp (`tools/rsxcheck`): 19 of 19 programs bit for bit. Semantics checked on ucode interpreters against CPU references (`make progtest`): 3,000 of 3,000 random XF states and 3,000 of 3,000 random TEV states |
| RSX backend `ps3/gx/backend.c` | Vertex loader (`vtx.c`: GX types kept where the RSX has one, indexed attributes, matrix indices), texture decoders (`texdec.c`: all 11 formats, TLUTs from a TMEM copy; mipmaps swizzled), texture cache by content, EFB copies to RSX textures, display copies to a virtual XFB, GX raster state. Device layer `rsxdev.h`: `rsxdev_cell.c` (PSL1GHT) and `tests/rsxdev_soft.c` (software, on the interpreters). `make scenetest`: the nine-cell frame through all of it on qemu-ppc64, PASS, 66 draws, 0 dropped, image as expected |
| UDP log, lv2 sync `ps3/os/` | `udplog_cell.c` (after DolphinPS3's `log.cpp`); `sync.h` with lwmutexes, lwconds and PPU threads on the PS3 (`sync_cell.c`) and pthreads on the host (`sync_host.c`). Both proven by the GX test EBOOT on hardware |
| GameCube addresses `ps3/os/mem.c` | `OSCachedToPhysical` and back over registered regions (`ps3_mem_add_region`), in 64 MB of GameCube physical space; the flipper's MMIO registers as plain memory |
| `os.h` clocks | Under `TARGET_PC`: `__OSBusClock` and `__OSCoreClock` are variables (162 and 486 MHz in `ps3/os/globals.c`) |

## Open decisions

- AXFX: write C versions of the three assembly effects, and change their 32-bit `long` sample types to `s32`
  (`AXAux.c`, `ax.h`, `axfx.h`, `axfx/*.c`). Safe for matching, but it edits SDK sources.
- `ftdata.c` lines 1763 and 1815 test `< 0x80000000UL` to read fighter data from ARAM. On the PS3 that is
  always true.
- The matching DOL is not verified after the source changes. Run `python configure.py && ninja` with a native
  `ninja`. Edits outside a `TARGET_PS3` block that must keep the MWCC output: `GX_WG(T, x)` at the direct
  pipe stores (`GXVert.h`, `__gx.h`, `GXInit.c`, `GXFifo.c`, `gm_1832.c`, `psdisp.c`), and `unsigned long`
  to `u32` in `__GXData_struct` (`__gx.h`). Under MWCC both are the same code.
- The OS layer must register the arena and any static data that GX reads with `ps3_mem_add_region`.
  `OSSleepThread`, `OSWakeupThread` and `OSInitThreadQueue` in `ps3/os/intr.c` are minimal, for
  `GXWaitDrawDone`; the OS layer's threads replace them.

## Findings

- A DVD or ARQ callback must not run inside the read call. `HSD_DevComDVDWakeUp` marks itself busy after the
  call returns, so a callback inside it stalls every later request.
- PSL1GHT is LP64. Sony's SDK is ILP32, which is why sagemono's port needs no `.dat` readers and this one
  does. On the PS3 all user addresses are below 4 GB, so a pointer survives a cast through `u32`, but not
  through `s32` at or above 0x80000000.
- PSL1GHT's `sbrk` reserves a 256 MB region with the lv2 memory mapper (syscall 330), above the ELF's first
  256 MB. Heap memory is therefore far above the 16 MB ARAM address range.
- PSL1GHT's `u64` is `unsigned long` and the SDK's is `unsigned long long`: no SDK header may meet a
  PSL1GHT header in one source file (`ps3/audio/audio_cell.c`).
- The SDK's `long` is 32-bit under MWCC. With LP64 it is 64-bit, which breaks the AUX buffers that the AXFX
  callbacks read.
- The SPU program keeps about 43 ms in the audio port. If a command list is late and less than about 11 ms
  remain, it writes silence instead of letting the port replay old data. Not measured on hardware.
- `__GXData_struct` had `unsigned long` fields, 64-bit with LP64. GX writes several of them through `u32*`
  (VAT, TEV stages, TEV order), which hit the high half. They are `u32` now.
- `GXTexObj` is 32 bytes, but the SDK's texture object holds a pointer: 40 bytes with LP64. Under
  `TARGET_PS3` it is `u64[5]`.
- GX packs texture, TLUT and copy addresses into 21-bit fields of 32-byte units (64 MB). Under `TARGET_PS3`
  the SDK gets each address from `OSCachedToPhysical`, and the GP turns it back with `OSPhysicalToCached`.
- Each GX store under `TARGET_PS3` appends to the ring: a limit check, then `memcpy`, as the stream is
  byte-packed and most stores are unaligned. The producer publishes every 16 KB and at `GXFlush`
  (`PPCSync`). Cost per store on the PPE: not measured.
- `GXInit` keeps the VAT registers in its shadow only; `GXBegin` sends the changed ones. The GP sees a VAT
  only after its first use.
- PSL1GHT's PTE `pthread_cond_wait` and contended `pthread_mutex_lock` spin on `sys_semaphore_trywait` (syscall
  93). Two waiting threads starved the main thread on hardware. `ps3/os/sync.h` uses lv2 lwmutexes, lwconds and
  PPU threads (priority 1001), as DolphinPS3. `ps3/dvd/dvd.c` and `ps3/audio/audio_cell.c` still use pthreads.
- Verified on hardware by the GX test frame (run 3): the NV40 vertex types 5 (s16) and 7 (u8) that PSL1GHT does not
  name; `GCM_FRONTFACE_CW` for clockwise front faces; the swizzled texture layout and mip levels; relative constant
  addressing (`c[A0 + n]`) for skinning; EFB copies to textures; the virtual XFB.
- RSX vertex programs have texcoord outputs 0-7 only: cgcomp's output 7 + n for n >= 10 is `result.clip`.
- cgcomp writes `ADD` with sources in slots 0 and 2, and scalar ops (RCP, RSQ, EX2, LG2) with the source in slot 2.
- GX front faces are clockwise on the screen. GXSetCullMode swaps FRONT and BACK in the register: 1 culls back faces.
- GX samples pixels at 7/12 of a pixel; the backend moves the viewport 1/12 pixel, as Dolphin.
- `GXInitSpecularDir` takes the direction the light travels; with `GX_AF_SPEC` the SDK sets the diffuse function to none.
- PSL1GHT has no line width command.
- sagemono measured about 1.4 µs of PPU work per vertex with GX lighting on the PPU, and 7-14 fps in matches.
