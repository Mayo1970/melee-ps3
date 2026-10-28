# Melee on PS3: goal and state

This file records the PS3 port of this decomp: the active goal, what is done, and what is open. A goal is done
only when the user reports `SUCCESS` from a real PS3.

## Rules

- Branch `ps3-port`. Port code lives in `ps3/`. Game and SDK sources change only where the port needs it,
  under `TARGET_PC` or `TARGET_PS3`, so the matching MWCC build keeps its code.
- Toolchain: Docker image `ps3dev-latest:latest`, PSL1GHT, `ppu-gcc` 7.2, LP64, `-O2 -mcpu=cell`.
  `ps3/build.sh` runs `make` in it. `ps3/build_linux.sh` runs the same Makefile with GCC 13 for ppc64
  big-endian ELFv1 (`ppc64-qemu-test:2`), for host tests under `qemu-ppc64`.
- The `.dat` schema: `docker build -t melee-datgen:1 ps3/tools/datgen`, then `python ps3/tools/datgen/extract.py
  Melee.iso orig/GALE01`, then `ps3/tools/datgen/datgen.sh` in that image (the command is in the script).
- Link with `-Wl,--no-multi-toc` and `-mminimal-toc` on the game objects (see Findings). Before a hardware
  test: `.got` below 0x10000, 0 `r2off` stubs, loaded segments within the RAM budget.
- Never write code against Sony's SDK. `E:\Users\Matteo\Desktop\Repo\melee-ps3` (sagemono/melee-ps3) is not
  the user's and has no license: use its findings, never its code.
- DolphinPS3 (`E:\Users\Matteo\Desktop\Repo\DolphinPS3`) is the user's: its RSX backend (`fpgen`, `TevGen`),
  pad, audio and QEMU harness may be reused. Dolphin code is GPL-2.0+.

## Active goal: Melee on the PS3

The game itself as the hardware test, with the UDP log. It replaces the separate test EBOOTs.

1. Link Melee into an EBOOT: done 2026-10-27 (`./build.sh -j12 game`, see Done).
2. Native `.dat` readers: done 2026-10-27 (`ps3/tools/datgen`, see Done).
3. Hardware test of Melee: the first screen, with the UDP log. The first screen and the title screen are
   reached, then the movie (green: no THP decoder), the main menu and the character select (run 13).

Earlier hardware tests:

- GX test EBOOT (`make gxeboot`, `ps3/tests/gx_scene.c`): run 1 black screen with no response; run 2 the UDP log
  stopped when the GP thread started (PTE waits spin, see Findings); run 3 (2026-10-27) SUCCESS: all nine cells
  as expected, 60 frames per second, 33 draws per frame, 0 dropped draws, 0 unknown opcodes, nothing unsupported.
- The boot, DVD, interrupt and audio test EBOOT is dropped for step 3.
- Melee run 1 (2026-10-27) FAILURE: back to the XMB. The log ran through OSInit (187 MB free at boot, 91 MB
  after the arena and the pool), the RSX at 1920x1080, the GP thread, the first retrace and the disc on the HDD,
  then ended in the game's assertion `dest % 32 == 0` in `HSD_DevComRequest`: `hsd_SynthSFXLoadBuf` was at
  0x1035b598. Fixed by `ps3/melee.ld` (see Findings).
- Melee run 2 (2026-10-27) FAILURE: back to the XMB. The game's banner came (Arena Size 48 MB, ARAM Free Size
  9 MB), then the assertion on the ARAM range in `lbMemory_80014E24` (`lbmemory.c`), from `lbHeap_80015900`
  in `lbDvd_80018CF4`: a main memory heap below 0x80000000 was taken for an ARAM heap. Fixed under
  `TARGET_PS3` in `lbmemory.c` and `ftdata.c` (see Done). The game's clock read 00:21 at 23:10 on the PC; the
  user sets the console's clock.
- Melee run 3 (2026-10-27) FAILURE: the user saw a crash. The log stopped after the game's banner, with no
  assertion and no exit line. Run 4 logs each path the game looks up (`dvd.c`) and a heartbeat once a second
  (`vi.c`: retraces, GP commands and draws, presents), to find where it stops and what still runs. The run 3
  log also ends in `melee: exit` 49 s after the banner, from the user's quit.
- Melee run 4 (2026-10-27) FAILURE, the user reports a console crash. The process lived 45 s: 60 retraces a
  second, the quit reached `ps3_sys_exit` (`melee: exit`). The GP got 0 commands, so no frame was drawn. The
  last paths are the memory card scene's (`LbMcGame.usd`, `NtMemAc.usd`, `NtMsgWin.dat`, `SdMsgBox.usd`); after
  them the game looked up no file. The main thread stops after loading that scene, before its first frame.
  After Quit Game the console froze (power cycle); after the panics of runs 1 and 2, the XMB came back.
- Run 5, UDP trace build: a second heartbeat line with counters (`ps3/os/diag.h`): the main thread's
  `OSDisableInterrupts` calls and `VIWaitForRetrace` calls, alarms fired, `PADRead` calls, CARD calls, DVD
  reads and callbacks, ARQ posts and callbacks. `ps3_sys_exit` logs again just before `exit()`.
- Melee run 5 (2026-10-27) FAILURE, the same as run 4. The main thread made 4,242 `OSDisableInterrupts`
  calls and 2 `VIWaitForRetrace` calls, then none in 36 s. The other threads ran: 60 alarms, 60 pad reads and
  180 CARD calls a second; all 204 DVD reads and 189 ARQ requests had their callbacks. So the main thread
  is blocked or dead after the memory card scene's loads, not in a game loop. The log reached "calling
  exit()", then the console froze: the freeze is in `exit()`, and only runs with this stopped main thread
  froze (runs 1 and 2 went back to the XMB).
- RPCS3 0.0.42 on this PC (`E:\Users\Matteo\Desktop\Emulatori\PS3`, the ISO hard-linked into its
  `dev_hdd0/game/MELE00001/USRDIR/games`): the EBOOT runs and its UDP log arrives from 192.168.178.99. After
  the same loads RPCS3 itself crashes (host segfault, its log cut off). `ps3/dat/trace.c` (`--wrap` on the
  archive calls) shows the last calls: `NtMsgWin.dat`'s `ScNtcCommon_scene_data` through
  `HSD_ArchiveGetPublicAddress`, then `SIS_MessageData` typed. `gm_801AEBB0` then reads
  `gm_804D6868->cameras[0].desc` as a native pointer. Cause: an untyped lookup returns the serialized bytes
  (upstream's `tools/dat-cli/README.md`: "Untyped lookups return serialized bytes"), and every `lbArchive`
  loader (`vLoadSections`) uses it. With LP64 the game reads 32-bit offsets as 64-bit pointers.
- Fix (2026-10-28): `ps3/dat/lookup.c` (`--wrap=HSD_ArchiveGetPublicAddress`) answers an untyped lookup with
  the typed native object when the schema has a root of that name, else with the serialized bytes (types
  without pointers and in the same layout). `yakumono_param`, the one name with several types, takes the
  type of the file whose roots the archive has most of. In RPCS3 the game then ran for 6 minutes: about 60
  presents and 62 draws a second, 0 dropped draws, the main thread waiting on retraces, then spinning in
  its loop (no pad input in RPCS3). A later RPCS3 run crashed RPCS3 itself after about 20 s.
- Melee run 6 (2026-10-28, the user's): the same build on the console rendered 184 presents in 3.5 s (11,422
  draws, 0 dropped), then every counter stopped: retraces, alarms, pad reads, the GP and the main thread.
  Only the heartbeat thread went on, for 61 s, with no `melee: exit`. Retraces and alarms need the
  interrupt lock, so a thread holds it while blocked. The GP's flip wait in `dev_present` has no timeout
  and logs nothing; the label waits log after 2 s, and none did.
  The user saw the same screen as in RPCS3, then the console froze.
- Run 7 diagnostic build: a third heartbeat line with the vblank count, what the GP thread waits for (ring
  data, a flip, an RSX label), whether the GX producer waits for ring space, the thread that holds the
  interrupt lock (`ps_thread_name`), the flips queued and done, the RSX GET and PUT, the fence labels, and
  the connected pads with the first pad's buttons. In RPCS3 it logs as expected (0 pads there).
- Melee run 7 (2026-10-28): the stall began as the user pressed Cross on the memory card notice. The
  interrupt thread held the interrupt lock; the GP and the RSX were idle. In RPCS3 an automatic A press
  (`PS3_PAD_AUTOPRESS`, RPCS3 build only) made the interrupt thread jump to an AX voice's address: the first
  sound effect. Three LP64 faults in the sound and movie code, fixed under `TARGET_PS3`:
  - `axdriver.c`: the `.sem` command table holds 32-bit addresses; `AXDriver_804D77BC[i]` read 8 bytes.
  - `synth.c`: the SFX bank and entry nodes keep the GameCube layout, linked by 32-bit words, and the code
    views them as `AXVPB` and `struct foo`; `struct foo`'s voice count read the wrong word and overran
    `voices[2]`. The links are `u32` now (`SFX_PTR`, `SFX_LINK`), and `HSD_SynthSFXBankDeflag` writes
    `hsd_SynthSFXBank[bank]`, which the GameCube reached by writing past `HSD_Synth_804C2AE0`.
  - `lbmthp.c`: the movie buffer reserved 4 bytes per `frame_buffers` pointer.
- RPCS3 after the fixes (trace build: `ps3/tests/rpcs3_trace.c` wraps the sound calls): the notice, the
  title, the intro movie, demo matches with fighters, the menus. Open there: an SFX bank overflow assertion
  in `HSD_SynthSFXHeaderLoadCallback` (`zs.ssm`, bank 2) after several demos, and RPCS3 crashing itself
  after a stage load.
- Melee runs 8 and 9 (2026-10-28, the user's, with the RPCS3 trace package): run 8 stalled at the first
  sound effect (before the `synth.c` fix). Run 9 reached the title screen and the intro movie; then the
  RSX stopped: GET stayed at 0x19c80 while PUT moved on, the fence labels stayed at 1519, and the GP waited
  for the RSX until the quit. The game went on loading the demo files.
- Run 10 build: the heartbeat adds the eight command words around GET when GET and PUT differ.
- Melee runs 10 and 11 (2026-10-28, run 11 with the 512 bytes before GET): the same stall at the first movie
  frame (`MvOpen.mth`). Two GX draws follow the title, then the RSX stops. GET stops in the setup of the next
  EFB copy pass, after its barrier (sync label 1008), and the label stays at 1007. So the RSX did not finish
  the two draws before that barrier: the movie's `HSD_SObjLib_803A4A68` quad (three I8 textures, two
  texgens, four TEV stages, three konst colours) is the suspect. GET only shows how far the RSX fetched ahead.
- The distortion the user saw (2026-10-28): `dev_present` set the right edge of the 4:3 quad to `1 - x0`
  (1.75 at 16:9) instead of `-x0`. Dumps in RPCS3 (`PS3_DUMP_FRAMES`, RPCS3 build only: XFB and display
  buffer to `/dev_hdd0/tmp`) showed a correct XFB and a stretched display; after the fix both are correct.
- Run 12 build: the RSX writes a draw label after each draw (`dev_mark`), and the backend keeps the last 16
  draws. At a stall the heartbeat logs the first draw the RSX has not finished: its attributes, textures,
  TEV stages, fragment program words and vertex program.
- Melee run 12 (2026-10-28): the stall again, at the movie's first frame. The RSX finished draw 55,379 of
  55,382. Draw 55,380 is not the movie quad but a GX quad just before it: position f32 x3, colour u8 x4,
  texcoord 0 as RSX type 7 (unscaled u8) with 2 components, a 4x4 texture, one TEV stage with the vertex
  colour. Nouveau's NV30 vertex table has type 7 only with 4 components; RPCS3 accepts any count, so it does
  not stop there. Fix: `vtx.c` sends GX u8 components as s16 (type 5, as s8 already), and type 7 stays for
  the matrix indices (4 components). `make scenetest`: PASS.
- Melee run 13 (2026-10-28): no RSX stall. The game passed the movie start, the main menu and the character
  select: 38 s, 1,889 presents, 435,986 draws, 0 dropped. It ended in the game's assertion in `synth.c` line
  295 (SFX bank 2 overflow) as `/audio/us/fox.ssm` loaded, after several Fox costume loads, then `OSPanic`
  and the exit to the XMB. The user: the picture is at 4:3 with side bars; the movie is green. The green is
  the THP stub (`ps3/sdk/stubs.c` decodes nothing): Y, U and V at 0 through the movie's TEV give green.

Waiting for the user:

- The title ID. The code uses the placeholder `MELE00001` (`PS3_APP_DIR` in `ps3/dvd/dvd.c`, `EBOOT_APPID` in
  `ps3/Makefile`).
- The next milestone: the SFX bank 2 overflow at the character select, or THP video decoding in C.

The UDP log goes to `192.168.178.99`, port 18194 (`ps3/os/udplog_cell.c`). A listener: `nc -ul 18194`, or any
UDP listener on that port.

## Not done for the game

| Part | Size or note |
|---|---|
| GX on RSX | Not drawn yet: indirect textures, TEV compare stages, two-compare alpha tests, logic ops, z textures, depth copies, emboss texgen, constant destination alpha, line width, a texcoord 7 matrix index in the vertex (the backend logs each once). Speed: a changed vertex program is uploaded again, vertices are de-indexed on the PPE for every draw (no display list cache), textures are hashed once per frame |
| CARD | Saves on the HDD; the stubs report no card in either slot |
| THP | Intro movie: `THPDec.c` has Gekko assembly; the stubs decode nothing |
| OS threads | `OSSuspendThread` only counts; thread queues (`OSSleepThread`) are the minimal ones in `ps3/os/intr.c` |
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
| ARAM addresses | On the GameCube an address below 0x80000000 is in ARAM. Under `TARGET_PS3` the test is `< ARGetSize()` (16 MB): `lbfile.c` (load target), `lbmemory.c` (ARAM heap range check, heap compaction), `ftdata.c` (fighter animations from ARAM, two places) |
| GX front end `ps3/gx/` | The SDK's GX library (16 sources, all Matching) builds for the PS3 and defines all 101 GX functions the game calls. Its stores go through a byte ring (`fifo.c`) to a GP thread (`gp.c`), which keeps the CP, XF and BP registers and runs display lists, draws, EFB copies, TLUT loads, tokens and draw done (PE interrupts) against a backend interface. Host test `gxtest` (qemu-ppc64): GP registers equal the SDK's shadow state; 642,248 vertices in 3,003 draws through a 64 KB ring with matching data hash, 6 of 6 runs; 100 `GXDrawDone` and one token by interrupt |
| RSX program generators `ps3/gx/` | `fpgen.c`, `tevgen.c` (from DolphinPS3, in C; TXP, EX2, RCP, fog, konst colours and TEV start values as interpolants, so the code holds no per-draw values); `vpgen.c` (NV40 vertex ucode); `xfgen.c` (XF to a vertex program: matrix-index transform, normals, GX lighting with spot and specular attenuation, texgen, post-transform matrices). Encoding checked against cgcomp (`tools/rsxcheck`): 19 of 19 programs bit for bit. Semantics checked on ucode interpreters against CPU references (`make progtest`): 3,000 of 3,000 random XF states and 3,000 of 3,000 random TEV states |
| RSX backend `ps3/gx/backend.c` | Vertex loader (`vtx.c`: GX types kept where the RSX has one, indexed attributes, matrix indices), texture decoders (`texdec.c`: all 11 formats, TLUTs from a TMEM copy; mipmaps swizzled), texture cache by content, EFB copies to RSX textures, display copies to a virtual XFB, GX raster state. Device layer `rsxdev.h`: `rsxdev_cell.c` (PSL1GHT) and `tests/rsxdev_soft.c` (software, on the interpreters). `make scenetest`: the nine-cell frame through all of it on qemu-ppc64, PASS, 66 draws, 0 dropped, image as expected |
| UDP log, lv2 sync `ps3/os/` | `udplog_cell.c` (after DolphinPS3's `log.cpp`); `sync.h` with lwmutexes, lwconds and PPU threads on the PS3 (`sync_cell.c`) and pthreads on the host (`sync_host.c`). Both proven by the GX test EBOOT on hardware |
| GameCube addresses `ps3/os/mem.c` | `OSCachedToPhysical` and back over registered regions (`ps3_mem_add_region`), in 512 MB of GameCube physical space (`GX_ADDR_BITS` 24 under `TARGET_PS3`); the flipper's MMIO registers as plain memory |
| `os.h` clocks | Under `TARGET_PC`: `__OSBusClock` and `__OSCoreClock` are variables (162 and 486 MHz in `ps3/os/globals.c`) |
| OS layer `ps3/os/` | `os.c`: `OSInit` (UDP log, clock, memory), arena, report and panic, reset (back to the XMB), console settings, contexts, cache barriers. `ostime.c`: the GameCube time base from the PPE time base and the local time. `heap.c`: OSAlloc's heaps with a 32-byte header that fits LP64. `alarm.c`: one alarm thread, handlers with interrupts disabled. `thread.c`. `sys_cell.c`: the lv2 side, free memory by container probe, XMB quit. `setjmp.S`: 64-bit `setjmp`/`longjmp` before newlib's |
| Memory | Arena 48 MB and `.dat` pool 48 MB (`ps3/dat/pool.c`), both from `memalign`; with the static data (0x10000000 up to `_end`), registered for GX |
| SDK shims `ps3/sdk/`, `ps3/pad/`, `ps3/vi/` | `mtx.c`: `PSMTX*`, `PSVEC*` and the C-only `mtx.c` calls, after the SDK's C versions; `mtx44.c`, `OSArena.c`, `PadClamp.c` as they are. `card.c`: no card. `stubs.c`: AXFX (effects off), THP, MCC, FIO. PAD: DS3 (Cross, Square, Circle, Triangle as A, B, X, Y; R1 Z; L2 and R2 the analog L and R, with a click at 230; right stick the C stick), the first four connected of seven ports. VI: the RSX vblank raises the retrace interrupt; a flushed XFB goes to the GP thread (`gp_post_present`), which presents between commands or while it waits |
| Game sources | `debug.c`: `HSD_LogInit` is empty under `TARGET_PS3` (newlib's stdout has no write hook). `hsd_3915.c`: `GX_WG`. Fonts: `tools/dol_inc.py` takes `HSD_DebugFontAtlas` and `HSD_SisLib_FontAtlas` from `orig/GALE01/sys/main.dol`, as `config.yml` names them. `src/MSL/float.c` for `MSL_TrigF_80400770` and `MSL_TrigF_80400774` |
| Final link (2026-10-27) | `make game`: the game, SDK and port objects, PSL1GHT libs, embedded SPU program; `EBOOT.BIN` and `melee.pkg`. Linker script `ps3/melee.ld`, `-fno-common`. `.got` 0x3f90, 0 `r2off` stubs; `.text` 9.8 MB, `.rodata` 2.3 MB, `.data` 0.8 MB, `.bss` 3.4 MB; loaded segments 18.0 MB. Main thread priority 1100, below the port's threads (1001): the game's loop spins between frames. `OSPanic` logs up to 16 return addresses from the back chain |
| `.dat` toolchain `ps3/tools/datgen/` | Docker image `melee-datgen:1` (`Dockerfile`: Debian trixie, clang 19, lld, Rust stable, Aurora e6a6f02, newlib headers). `extract.py` writes `orig/GALE01/files` and `sys/main.dol` from the ISO (the DOL's SHA-1 matches `build.sha1`). `datgen.sh`: the PPC DWARF build, `melee-dat`, `types.bin` (1.9 MB), `native codegen` into `ps3/build/datgen/schema` (163 C files, 22 MB) |
| Native `.dat` readers | `tools/dat-cli/native` and the schema in the EBOOT, with `-mminimal-toc`, allocations in the `.dat` pool (`ps3/dat/redirect.h`). `make dattest` (TOOLCHAIN=linux, qemu-ppc64, the EBOOT's headers and flags): upstream's HSD test passes, with typed dispatch, externs and 27 fighters |

## Open decisions

- AXFX: write C versions of the three assembly effects, and change their 32-bit `long` sample types to `s32`
  (`AXAux.c`, `ax.h`, `axfx.h`, `axfx/*.c`). Safe for matching, but it edits SDK sources.
- The matching DOL is not verified after the source changes. Run `python configure.py && ninja` with a native
  `ninja`. Edits outside a `TARGET_PS3` block that must keep the MWCC output: `GX_WG(T, x)` at the direct
  pipe stores (`GXVert.h`, `__gx.h`, `GXInit.c`, `GXFifo.c`, `gm_1832.c`, `psdisp.c`, `hsd_3915.c`),
  `unsigned long` to `u32` in `__GXData_struct` (`__gx.h`), and `GX_ADDR_BITS` (21 there) in `GXTexture.c` and
  `GXFrameBuf.c`. Under MWCC all are the same code.
- The upstream PC build (Aurora's headers) has no `GX_WG`, so `gm_1832.c` and `psdisp.c` do not compile there.
  `datgen.sh` forces `ps3/tools/datgen/gx_wg.h` into the DWARF build.
- `OSSleepThread`, `OSWakeupThread` and `OSInitThreadQueue` in `ps3/os/intr.c` are minimal (one condition for
  all queues); `GXWaitDrawDone` and `VIWaitForRetrace` use them.

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
  PPU threads (priority 1001), as DolphinPS3. `ps3/dvd/dvd.c` and `ps3/audio/audio_cell.c` use it too.
- Verified on hardware by the GX test frame (run 3): the NV40 vertex types 5 (s16) and 7 (u8) that PSL1GHT does not
  name; `GCM_FRONTFACE_CW` for clockwise front faces; the swizzled texture layout and mip levels; relative constant
  addressing (`c[A0 + n]`) for skinning; EFB copies to textures; the virtual XFB.
- RSX vertex programs have texcoord outputs 0-7 only: cgcomp's output 7 + n for n >= 10 is `result.clip`.
- cgcomp writes `ADD` with sources in slots 0 and 2, and scalar ops (RCP, RSQ, EX2, LG2) with the source in slot 2.
- GX front faces are clockwise on the screen. GXSetCullMode swaps FRONT and BACK in the register: 1 culls back faces.
- GX samples pixels at 7/12 of a pixel; the backend moves the viewport 1/12 pixel, as Dolphin.
- `GXInitSpecularDir` takes the direction the light travels; with `GX_AF_SPEC` the SDK sets the diffuse function to none.
- PSL1GHT has no line width command.
- `gx_test` on qemu-ppc64 (2026-10-27): 1 of 9 runs hung until the 120 s watchdog; a passing run takes 8 s.
  The cause is not found yet.
- `ppu-nm` on `melee.elf` lists the `.opd` descriptors, not the code. To name a return address from the
  `OSPanic stack:` line, read the code addresses from the `.opd` words (`ppu-objdump -s -j .opd`).
- The game expects 32-byte aligned static buffers for DMA and GX that the DOL's layout gave without an attribute
  (`hsd_SynthSFXLoadBuf` is at 0x804C2AC0 there). With lv2.ld, 10,375 of the 11,248 static objects of 32 bytes or
  more were not 32-byte aligned. `ps3/melee.ld` (lv2.ld with `SUBALIGN(32)` on `.rodata`, `.data` and `.bss`) and
  `-fno-common` align all of them; `-T` replaces lv2.ld in ppu-gcc's specs.
- The schema comes from the DWARF build against Aurora's headers; the PS3 build compiles it against the SDK's.
  Only `dolphin/mtx/GeoTypes.h` and the `struct Vec` tag differ (`ps3/dat/include`).
- lv2.ld places `.rodata` at 0x10000000, then `.data` and `.bss`, and aligns `_end` to 256 MB, so the static
  data region takes 256 MB of physical space. No linker symbol marks the end of `.bss`.
- sagemono measured about 1.4 µs of PPU work per vertex with GX lighting on the PPU, and 7-14 fps in matches.
