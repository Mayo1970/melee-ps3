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
- The SFX bank 2 overflow (2026-10-28): the RPCS3 trace (`build/rpcs3_trace_run.txt`) shows `dk.ssm`,
  `nr_select.ssm` and `peach.ssm` each loaded twice into bank 2. `lbAudioAx_80027168` starts the background
  chain with `lbl_80433A64[slot] = HSD_SynthSFXLoad(..., fn_80026C04)`. On the PS3 the DVD thread (1001) runs
  ahead of the main thread (1100), so the whole load ends in the `OSRestoreInterrupts` of `HSD_SynthSFXLoad`,
  before the store. `fn_80026C04` then finds no slot with that entrynum, and loads the same file again. The game
  counts each file once, so bank 2 fills. On the GameCube the disc latency hides this. Fix under `TARGET_PS3` in
  `lbaudio_ax.c`: the slot gets `DVDConvertPathToEntrynum` before the call. Inside a callback the load only
  queues, so `fn_80026C04` needs no change. The build passes (`.got` 0x4180, 0 `r2off` stubs).
- Melee run 14 (2026-10-28, the user's): the bank 2 fix works. Each `.ssm` loads once, and the character
  select, the stage select and the match load pass with no assertion. After `ItCo.usd` (`itPublicData`) in
  `fn_8016E730` the main thread stops: from then on it makes 0 `OSDisableInterrupts` calls and 0 retrace waits,
  and nothing is presented. The other threads run on. The user reports a crash; the log ends 37 s later
  with no `melee: exit`. From the main menu on, DVD reads (about 940 a second) and ARQ posts (about 750 a
  second) never stop: about 188 music stream blocks (hako) a second, one per AX frame. Cause: an LP64 fault.
  `HSD_SynthPStreamMasterClockCallback` reads the voice's play position at byte 0x1B2 of the `AXVPB`, which is
  `pb.addr.currentAddressHi` on the GameCube. With 64-bit pointers, `pb` is at 0x158 instead of 0x138, so the
  game read the wrong field and loaded the next block on every AX frame. `HSD_Synth_804D7778` (a block load is
  in progress) was then set almost all the time. `HSD_SynthPStreamStart` spins on it with interrupts on
  (`do {} while (HSD_Synth_804D7778 != 0)`), and the match starts the stage music, which fits the stop. The
  log does not show where the main thread is. Fix under `TARGET_PS3` in `synth.c`: the read uses the field
  name (offset 0x1D2 in the object). The build passes (`.got` 0x4180, 0 `r2off` stubs).

- Melee run 15 (2026-10-28, the user's): the music plays right, about 5 DVD reads a second. The match still
  stops: on Green Greens after `PlCo.dat`, on Yoshi's Story (run 16) after `itPublicData`.
- Call trace build: `./build.sh -j12 game BUILD=build/ppu_calltrace "EXTRA_CFLAGS=-finstrument-functions
  -finstrument-functions-exclude-file-list=ps3/,tools/,baselib/card.c"` (`card.c`: its dead `CARDRead` calls
  stay alive). `ps3/os/calltrace.c` keeps the main thread's open calls and last 8 entries; the heartbeat logs
  them as `.opd` addresses (`ppu-nm -n` into `build/calltrace_syms.txt`).
- Melee run 17 (2026-10-28, call trace, Yoshi's Story): the main thread stops in `HSD_AObjLoadDesc`, from
  `grAnime_801C7C1C` (`Stage_8022524C`, `grStory_801E3234`), with no later call: a fault on `aobjdesc`.
  `grAnime_801C7C1C` takes `&aj[arg2]` (also `mj`, `sj`): the stage's anim joints lie in one array in the
  archive, but the native reader allocates each joint on its own, so the index left the object. Fix under
  `TARGET_PS3`: `ps3_dat_element` (`ps3/dat/lookup.c`, `ps3/include/ps3_dat.h`) finds the joint's serialized
  offset and gives the native object `arg2` elements on. Host check (qemu-ppc64, GrSt, GrGr, GrCs, GrIz, GrMc):
  each element is the node at that serialized offset, as on the GameCube (DFS order where the data is
  contiguous; GrMc has one tree with gaps). Both builds pass (`.got` 0x41a0 and 0x4fd8, 0 `r2off` stubs).
- Melee run 18 (2026-10-28, call trace): Yoshi's Story passes the stage animations, then stops in
  `lbFileGetFullName` from `Fighter_Create` → `ftData_8008572C`. Run 19 (`ps3/tests/match_trace.c`, `--wrap`
  on `Player_80031AD0`, `Fighter_Create`, `ftData_8008572C`): player 0 is Peach (character 12, kind 9), and
  `ftData_803C1F40[9].a` is NULL at that point. The table is static data, so something writes over it. The
  next trace build watches that word (`ps3_diag.watch`, checked at each traced call on any thread).
- Run 20 watch (2026-10-28): the writer is `ft_800852B0`, which reaches `ftData_Table_Unk0`,
  `ftData_UnkIntPairs` and `ft_8045993C` by their DOL offsets from `CostumeListsForeachCharacter` and
  `gFtDataList`. On the PS3 the first offset lands on `ftData_803C1F40` and clears file names 0-31. Fix under
  `TARGET_PS3` in `ftdata.c`: the named arrays.
- Melee run 21 (2026-10-28, normal build): `PlPe.dat`, `EfPeData.dat`, `PlPeNr.dat` load, then the assertion
  "can't find tobj!" (`ftparts.c` 799) from `ftAnim_80070200` in `Fighter_Create`. Cause: `ftData_x8_x8::xC`
  is `u16**` with no annotation, so the reader converts one costume list of one index; `x5D0[i]` for i > 0
  reads past it. Needs annotations and a new schema.
- Host game build (2026-10-28, see below): done. It reproduces run 21 ("can't find tobj!" from `PlPe.dat`).
  After the annotations and a new schema, the Peach match passes `Fighter_Create` and the host build found
  more LP64 faults, fixed under `TARGET_PS3` unless noted:
  - `ft/types.h`: `ftData_x8_x8::xC` is a list per costume (`FtCostumeTObjIndices`, `DAT_EXTENT`, bound
    count). `ftData_80085FD4_ret` views `Fighter_WaitAnimData` entries: its `x4` and `x8` are `s32` (the
    motion size was read from the script pointer, and `memcpy` faulted).
  - Items (audit findings 2-4): `ItemDynamics` has the second pair (`coll_count`, `coll_descs`), and
    `ItCollDynamics` names the first pair's words, so its fields keep their place; `ItHurtBoneList::descs` has
    `DAT_COUNT(count)`; the G&W attributes' `x0` is `struct it_266F_ItemVars*`, whose lists have counts. These
    type changes are outside `TARGET_PS3` and keep the MWCC layout.
  - `fighter.h`: `ftLoadCommonData::x20` is the respawn platform (`ftLoadCommonData_x20`), not `UNK_T`.
  - Particles (`particle.c`): a bank is 32-bit words, relocated in place on the GameCube. Its tables
    (`ptclref_804D0E5C`, `psTexGroupArray`, `psCmdListArray`) were read as 8-byte pointers. Locate relocates
    the words as words; Load gives the bank native copies of the tables and groups (freed by the next Load).
  - DOL layout reads, as `ft_800852B0`: `Camera_ApplyQuake` (`cm_803BCB64` from `cm_803BCB18`: NaN camera,
    `lbvector.c` 383), `ftMaterial` (two templates from `ftMObj`: `texp.c` 1048), `lbRefract_800222A4`
    (`imagedesc0` from `texture_mtx`).
  - `ground.c`: `Ground_801C0FB8` read the callback list with a 32-bit `unk4`, which `Ground_801C10B8` writes
    as a GObj pointer.
  - Command scripts: `Command_04` (loop) reached its count through 32-bit words from `CommandInfo`.
    `Command_05` and `Command_07` (subroutine, goto) read 8-byte pointers: the native readers keep scripts as
    serialized bytes, so a script pointer is a 32-bit offset into the archive's data. They are `u32` now
    (`lb/types.h`), resolved by `ps3_dat_script` (`ps3/dat/lookup.c`), and `CmdUnion` is 4 bytes again.
  - `.dat` pool leak: each fighter motion parses its animation archive into the same buffer
    (`ftData_80085E50`), and each parse kept a native copy, so the 48 MB pool filled in under a minute of
    match. `ps3/dat/lookup.c` wraps `HSD_ArchiveParse` and `HSD_ArchiveReleaseNative`: a parse frees the
    native data of the archives whose buffers it overlaps.
- Host matches after the fixes (CPU level 9 against an idle player 0, 45-90 s each, `tools/hostmatch.sh`): Peach
  and Fox on Yoshi's Story, Green Greens and Final Destination, Mario and Link on Battlefield, run 90 s with no
  fault. With items at frequency 4: 10 of 13 fighter pairs pass on Final Destination (all 26 fighters
  load); 17 of 30 stages pass with Mario and Fox. Open, from the logs (`build/hostmatch/`):
  - Kirby and Bowser: assertion `ftparts.c` 458 in `ftParts_SetupParts`.
  - Zelda and Sheik, Dr. Mario and Roy, Mute City (10), Yoshi's Island (16), Fourside (18): assertion
    `rvalue->jobj` (`robj.c` 894) in `HSD_RvalueResolveRefs`.
  - Faults in stage code: Izumi (2) `Ground_801C1D38`, Castle (4) `grCastle_801CF308`, Kongo (5)
    `grKongo_801D77E0`, Zebes (6) `it_802756E0` from `grMaterial_801C8E08`, Rainbow Cruise (11) `ps_heap_alloc`
    from `Ground_801C10B8`, Inishie2 (20) `grInishie2_801FDED8`, Akaneia (21) `Ground_801C0754`, Venom (22)
    `grVenom_80203EAC`; Big Blue (24) `memory.c` 23 (`HSD_MemAlloc` fails in `HSD_JObjLoadJoint`);
    Pokemon Stadium (3) glibc's "buffer overflow detected" after a Fox figatree.
  - Not done: item audit findings 1 and 5-7, the `ft/` tether views, `ftCo_ItemScopeRapid.c` `+0x2340`.
- Both packages build (2026-10-28): normal `.got` 0x41a8, call trace `.got` 0x4fe0, 0 `r2off` stubs.
- Melee run 22 (2026-10-28, normal build): the menus pass; Peach and Captain Falcon on Yoshi's Story, chosen
  through the menus, stop the main thread after `GrSt.dat` loads, with no assertion. The host build runs that
  match (all costumes) with no fault.
- Melee run 23 (call trace): the main thread stops in `ps3_tex_words` from `ps3_bank_load`
  (`psInitDataBankLoad`, `grDatFiles_801C6038`). Cause: through the menus the stage is preloaded, and the
  preload callback `grDatFiles_801C5FC0` locates the banks of a typed lookup (`HSD_ArchiveGetPublicAs(int,
  ...)`, a native copy), while `grDatFiles_801C6038` loads the serialized banks, never located. The host's
  debug VS start does not preload, so `psInitDataBank` locates and loads the same banks. Fix under
  `TARGET_PS3` in `grdatfiles.c`: the callback takes the serialized banks too. Both packages rebuilt.
- Melee run 24 (2026-10-28, normal build): Training mode, Peach and Captain Falcon on Yoshi's Story. The stage
  and fighters load, "use dummy CamRange" and "use dummy DeadRange" show, then `ps3_dat_malloc: no room for
  65536 bytes` at `GmTrain.usd`, and back to the XMB. The 48 MB `.dat` pool was full. The host build's debug VS
  match on Yoshi's Story peaks at 39.9 MB: the reader copied each archive's data into the pool
  (`dat_archive_open`), and the native objects take the rest (82,000 live blocks, 76,000 of them 64 bytes or
  less). Fixes: under `TARGET_PS3` in `tools/dat-cli/native/src/archive.c` the reader keeps the data in the
  game's buffer, as the GameCube does (32.1 MB on the host); the pool is 64 MB (`ps3/dat/pool.c`). Each parse
  logs the pool's use and peak. Both packages rebuilt (`.got` 0x41b8 and 0x4ff0, 0 `r2off` stubs).
- Melee run 25 (2026-10-28, normal build): Training, Peach and Captain Falcon on Yoshi's Story. The match runs
  about 57 s (pool peak 36.4 MB), then the main thread stops with no message; the user saw wrong standing
  animations and Ice Climbers' icon for both players. Causes found:
  - The stop: `lbArq_80014BD0` waits for a synchronous ARAM read with `while (lbArq_80014ABC(rp) != DONE)`.
    MWCC keeps `lbArq_80014ABC` out of line (`#pragma dont_inline`); GCC inlines it and loads `state` once,
    before the loop (`ppu-objdump`: `lwz`, then `cmplwi`/`bne+` back to the compare). Fix under `TARGET_PS3`:
    a volatile read. A scan of the EBOOT for compare-and-branch loops with no load found no other one.
  - The icons: `gm_80168B34` sets `base` for some fighters only; the DOL keeps it in `ckind`'s register (r3),
    so it is `ckind` for the others. GCC left it undefined (14, Ice Climbers). Fix under `TARGET_PS3`. Host
    frames show Peach's and Captain Falcon's icons.
  - The standing animations: not found yet. The host frames show Peach and Captain Falcon standing as
    expected. Leads: GCC 13 reports 188 "may be used uninitialized" in the game (`build/linux_uninit`, with
    `-Wmaybe-uninitialized`); `resolveIKJoint1` (`jobj.c`, leg IK) uses `var_f27` and `var_f28` unset on one
    path, as the DOL does (callee-saved f27 and f28); `HSD_RvalueResolveRefs` fails on some fighters on the host.
- Host frame dumps: `MELEE_HOST_DRAW=2` draws only the 3 frames before each dump, `MELEE_HOST_DUMP_FROM` skips
  the boot; `tools/ppm2png.py` converts the dumps. A frame takes about 10 s under qemu.
- Melee run 26 (2026-10-28, normal build): Training, Peach and Link. The user: after an idle animation, Peach's
  and Captain Falcon's limbs stay deformed instead of going back to standing; Toad's counter freezes the game.
  The log ends with Peach `SpecialLwHit` (the data names Toad `SpecialLw`) and Link `DamageFlyTop`, then the
  main thread stops, as in run 25. The host does not freeze (14 counter hits against a level 9 Link).
- Melee run 27 (call trace build): the attract demo (Kongo Jungle: Bowser, Kirby, DK, Fox) stops at
  `ftparts.c` 459 "fighter parts num not match", as the host's Kirby and Bowser match. Cause:
  `Fighter_804D6540_t::x0` (the parts `ftParts_8007506C` skips) had no count; fixed with `DAT_COUNT(x4)`
  (`fighter.h`), schema regenerated. Then Training, Fox and Bowser: the main thread stops in `hsd_8039D0A0`
  (`efLib_particles_proc_main`, `hsd_8039EE24`, `hsd_8039D3AC`): it reaches the particle lists and their
  allocator through `ParticleData` laid over `psPointJObj`, a DOL layout read (`hsd_804D0908`, `hsd_804D0F60`).
  Fixed under `TARGET_PS3` in `particle.c` (`PD_JOBJ`, `PD_PARTICLE`, `PD_ALLOC`). The Toad freeze is likely
  the same (its spores' particles); not confirmed.
- State when stopped (2026-10-28): the `fighter.h` and `particle.c` fixes are in the sources and the schema; the
  PS3 packages are rebuilt with them (`.got` 0x41b8 and 0x4ff0, 0 `r2off` stubs); the host build is not.
  `tests/sys_host.c`: SIGUSR1 at the quit logs the main thread's place; its `ps3_log` call had `%s` and `%d`
  arguments swapped (the host runs faulted at the quit); fixed, not yet built. `tests/host_start.c`: `ckind:color` picks a costume, a trailing `h` makes a human.
  `tools/hostmatch.sh` takes `MELEE_HOST_PAD` from the environment.
- GameCube-call audit (2026-10-28): six read-only audits of the game and SDK sources, then the fixes, all under
  `TARGET_PS3` with the GameCube code in `#else` (or annotations, which MWCC does not see). The audits: the 310
  int and pointer cast warnings; MWCC-only code and GCC undefined behaviour (286 `MUST_MATCH`/`MWERKS_GEKKO`
  blocks, 41 `-Wreturn-type`, 29 `-Warray-bounds`, busy-waits); DOL layout views and raw offsets; storage shared
  by pointers and 32-bit values, and literal sizes; the SDK call surface; `.dat` pointer fields with no count.
  The main results:
  - Idle animations (runs 25 and 26): `getAnimID` (`ftwaitanim.c`) read `WaitStruct`'s 8-byte `u.p.x`, which
    is the chance, as the motion id. Peach's table is {2,25} {3,25} {4,25} {5,25}, so every pick was motion 25
    and the retry loop never ended: the main thread stop with no message.
  - The five `rvalue->jobj` assertions (`robj.c` 894): the Fire Flower's 16 bytecode RObjs; `HSD_ExpDesc` and
    `HSD_ByteCodeExpDesc` `rvalue` lists are `DAT_TERMINATED(0)` now. `bytecode.c` keeps a stack word in the
    first 4 bytes of the 8-byte slot, where `ByteCodeVal` reads it.
  - Stage faults from the host matches: Venom (`grVe_803E5348` read as one DOL array: callbacks, Arwing GObjs,
    animation tables), Kongo (a 0x10 stride over a pointer), Castle (`castle7.xD0` over `castle11.xD4/xD8`),
    Big Blue (a 120-byte block for 30 pointers, about 30 raw Ground offsets, the `rlwimi` car state that only the
    `MUST_MATCH` asm set), Pokemon Stadium (a 0x1C block for a 40-byte struct), Zebes (bubble statics by DOL
    offset, the acid overlay), Izumi (a GObj written into `xC_callback`), Inishie2, Rainbow Cruise (a 160-byte
    block for 320 bytes), Icicle Mountain (`DAT_TERMINATED(-1)` on three lists), Home-Run, Flat Zone, Green
    Greens, Mute City and Big Blue Route (`UnkStageDat_x8_t` stride 0x34).
  - Fighters: colour overlays (`ftdevice.c` read the low half of a union), Bowser's side B and up B
    (`ftKoopa_State1Vars`, `common.x10`), Kirby's copies (part masks, hat DObjs at a 4-byte stride, the copy table
    cleared to half), capture and thrown states (Koopa, Kirby, Yoshi egg), the damage log `x1870`, throw
    velocities, Yoshi's shield parts, Super Scope (`+0x2340`, `+0x2C`, a missing return), shake tables and item
    throws (`DAT_COUNT`).
  - Results screen, Classic, All-Star, Home-Run Contest, Multi-Man, tournament, event matches, credits,
    1P splash: statics reached by DOL offset or raw offsets into structs with pointers. Menus: name entry (a
    0x44 block for 120 bytes), data delete, rules (`mn_80231634` read `next`'s high half), event, info, counts
    (a string written below `buf`). Trophies: one block for `_Toy_804A26B8`..`Toy_804A2AA8`, the archive table
    at half its size, allocations, about 20 view structs over GObjs, SObjs and list entries.
  - Items: Starman time, Link's arrow, the random item tiers, the Fire Flower (above).
  - MWCC behaviour: `lbMthp_8001F800` waited on `unk_110` with one load (a branch to itself, as `lbArq`);
    `fn_80166A8C` had only the `psq_st` asm (GQR3: u16, scale 0); the particles attached to joints never spawned
    (`jobj.c` set bit 31 of the pointer's high half).
  - SDK: async DVD reads go to a read thread (`dvd.c`). Read in the call, a devcom callback chain kept the
    interrupt lock for a whole file: 10 alarms a second during loads in runs 22-26. Pad channels stay with their
    port (`pad_cell.c`). `DBIsDebuggerPresent` is TRUE, so an assertion goes straight to `OSPanic`; the game's
    crash screen thread spun on during `exit()` and wrote to a dead stack frame.
  - Not changed: pointers kept in `int`/`s32` (safe below 0x80000000: heap and static data are), and debug-only
    code (`debugconsole_main.c`, `leak.c`, `hsd_393C.c`) that `dberror.c` no longer reaches.
  - Host matches after the fixes (items at frequency 4, 45 s each): 43 of 43 pass, that is the 13 fighter pairs
    on Final Destination, Mario and Fox on stages 2-31 but 21 (Akaneia: no stage data, not playable), and an idle
    Peach on Yoshi's Story. The same list passed 25 of 43 before. The run found three more, fixed: Nana copies
    Popo's motion file and relocates it with `lbArchiveRelocate`, which makes no native data (now parsed, in
    `ftdata.c`); Zebes reads `unk8[2]`'s animations at raw +0x6C..0x74; the text printf's `vsnprintf(buf, -1)`
    aborts in glibc's checks on the host.
  - Both packages rebuilt (`.got` 0x4408, 0 `r2off` stubs, 17.5 MB in loaded segments). Host boot over the
    movie, 150 s: 4,744 DVD reads with their callbacks through the read thread, about 100 alarms a second.
- THP (2026-10-28): `ps3/sdk/thp.c` decodes the frames in C with the SDK decoder's calls, work area and output (GX
  I8 tiles). The frames are baseline JPEG, 4:2:0, with no 0x00 after an 0xFF in the scan. MvOpen.mth is 640x480 at
  30 fps (3,036 frames), the endings 448x336, the stills (`GmRegend*.thp`) 560x416. `make thptest` writes frames
  as PPM and as standard JPEG; the PPM matches the JPEG decode. Host boot: the intro movie shows in the game.
  The GX texture cache hashes 1 KB of a large texture, so each decoded plane is posted to the GP
  (`gp_post_mem_changed`, a port opcode 0x7E) and its cached textures are freed. Under qemu a 640x480 frame takes
  17 M instructions.
- Frame rate (2026-10-28): in the run 26 match the game drew 19 frames a second while its logic ran at 60 (the
  pad queue runs several logic frames per drawn frame). Menus draw at 60. The GP thread is the limit: it never
  waits in a match, while the main thread waits for a free XFB. A frame has about 2,100 GX draws, each one RSX
  draw, about 24 µs each on the GP thread. A host match profile (qemu plugin, instructions per thread): the GP
  thread runs 7 times the main thread's instructions, about 18,600 per draw. Of the GP time: TEV config and its
  688-byte hash 23%, vertex conversion 20%, constants 14%, RSX commands 10%, textures 9%, state 5%, XF key 4%,
  command parsing 14%. 79% of the draws have no CP, XF or BP write since the draw before. Not measured under
  qemu: `xf_constants` converts all 225 constant rows through memory (load-hit-store) on each XF change (6,900 a
  second), the FNV hash is a chain of multiplies, `gp_fifo_release` does two `sync` atomics per command, each
  draw invalidates the vertex cache and writes a backend label, a new vertex program is uploaded each time
  (1,700 a second), and `finish` waits for the RSX at each draw done. The heartbeat's fourth line gives the
  same split on the console.

### Host game build (big-endian LP64)

The EBOOT's sources as a Linux ppc64 big-endian ELFv1 program (`TOOLCHAIN=linux`, GCC 13, `ppc64-qemu-test:2`),
under `qemu-ppc64 -R 0x100000000` (a 4 GB guest space, as the PS3's user addresses). Pointer size, byte order
and the native `.dat` readers are as on the PS3. Not for: the RSX and NV40, lv2 scheduling and timing, the
SPU's timing, speed. Under qemu a match runs at 20-35 presents a second.

- Build: `./build_linux.sh -j12 hostgame` (`build/linux/game/melee_host`). The PSL1GHT side is replaced by
  `tests/sys_host.c` (clock, local time, exit, a fault handler that logs the back chain), `tests/pad_host.c`,
  `tests/audio_host.c` with `audio/ax_spu.c` on a host thread at 48 kHz (`HOST_AUDIO_REALTIME`),
  `tests/rsxdev_soft.c` (a first-fit local memory allocator, a 59.94 Hz vblank thread, draws dropped unless
  `MELEE_HOST_DRAW=1`), `os/sync_host.c`, `tests/udplog_host.c` (stdout, line-buffered). The link script is
  the default one with `SUBALIGN(32)` on `.rodata`, `.data` and `.bss`, as `melee.ld`. glibc's `setjmp`.
- Run: `ps3/run_host.sh` (the disc from RPCS3's `MELE00001/USRDIR/games`, mounted at `/scratch`; only the ISO
  file is read, RPCS3 does not run). Settings by environment: `MELEE_HOST_SECONDS` (quit, exit status 0),
  `MELEE_HOST_PAD` (pad 0 script, e.g. `200:A,300:A,400:A` for the memory card notice), `MELEE_HOST_MATCH`
  (`tests/host_start.c`: `ckind,ckind/stkind/item_freq`; the boot goes to the debug VS mode with these, player
  0 human, the others CPU level 9), `MELEE_HOST_DRAW`, `MELEE_HOST_DUMP` (PPM prefix), `MELEE_HOST_DUMP_EVERY`.
- Without a match, the boot reaches the notice, the title, the movie, a demo match on Pokemon Stadium and the
  main menu, as on the console.
- `tools/hostmatch.sh` runs matches 4 at a time, a log each in `build/hostmatch`. `tools/hostsym.sh` names
  addresses of a fault or `OSPanic stack:` line (`addr2line` on `melee_host`). The image has no gdb.
- Item audit (2026-10-28, read only, not run). Findings 1-3 were checked against the code; 4-7 were not.
  Findings 2-4 are fixed (above).
  1. `it_2725.c:117` `it_80272818`: the Starman's time is read as a `Fighter*` (`unk_2728`), cast to `int` in
     `ftcoll.c:884`.
  5. `itlinkarrow.c:678,686`: GameCube strides into `it_803F6A28`; use `it_803F6A84` as line 128 does.
  6. `it_2E5A.c:338`: `tier_thresholds[9]` steps between tiers; use `tiers[1].threshold`.
  7. `it_2E5A.c:438`: part of a tier entry cast to `ItemStateDesc`; safe until `Item_80268E40` uses it.
  Also: the `tether` and `chain_segment` views in `ft/`; `ftCo_ItemScopeRapid.c:145,154` reads
  `(u8*) fp + 0x2340`, a GameCube offset into `Fighter`.
- "use dummy CamRange" and "use dummy DeadRange" do not show in the host matches on Green Greens and Yoshi's
  Story.
- To name an `OSPanic stack:` address: `ppu-objdump -s -j .opd` and `ppu-nm -n` on `melee.elf`, map each
  descriptor to its code address, then take the function at or below the address.
- `make dattest` no longer links: `ps3/dat/lookup.c` needs `-Wl,--wrap=` for `HSD_ArchiveGetPublicAddress`,
  `HSD_ArchiveParse` and `HSD_ArchiveReleaseNative`.

- Melee run 28 (2026-10-28, two boots in `build/run25_udp.txt`, which the listener appends to):
  - 16:39, the first package of the audit (without the Ice Climbers, Zebes and `vsnprintf` fixes): the movie,
    then Peach and Captain Falcon on Yoshi's Story for 5 minutes until the user quit. Idle animations played
    with no stop: Peach Wait2-4 22 times, Captain Falcon Wait2-3 61 times (the run 25 and 26 stop is fixed).
  - 17:00, the final package: the movie for 34 s at 32 presents a second (GP 59% in texture uploads, main
    thread 69% busy), a demo on Castle, Peach and Bowser on Yoshi's Story for 56 s, then Peach and DK there.
    After 57 s the GP thread stopped with no wait state, the RSX idle (GET = PUT, labels done), and the main
    thread slept in `GXWaitDrawDone` for good (its disables stopped, its sleep never ended). The other threads
    ran on until the user quit. The last loads: DK SpecialLwStart and SpecialLwLoop, Peach JumpF. Not
    reproduced on the host (DK down-B 182 times by pad script, Peach and DK as CPU or human).
  - Frame rate in that match: 12 presents a second, 48,000 draws a second (4,000 a frame), 83% with no
    register write since the draw before. GP per mille: draws 866 (TEV 319, vertices 176, textures 124,
    constants 87, RSX commands 75, XF 41, state 41), RSX label waits 7. Main thread asleep 705: the GP limits.
  - The heartbeat's main-asleep share counts only sleeps that end, so a sleep that never ends shows 0.
- Run 29 build (call trace): a fifth heartbeat line gives the GP thread's step (`ps3_diag.gp_site`, set in
  `gp.c` and at each draw phase in `backend.c`), the last opcode and draw, and the queue the main thread
  sleeps on.

Waiting for the user:

- A test of the GC adapter with the run 61 package (the user will do it another time).
- The title ID. The code uses the placeholder `MELE00001` (`PS3_APP_DIR` in `ps3/dvd/dvd.c`, `EBOOT_APPID` in
  `ps3/Makefile`).
- Run 29: the call trace package, Peach and DK on Yoshi's Story until the stop, and what was on the screen at
  the stop and in the movie.
- Run 30: the normal package with step 1 (dirty tracking), a match, and the heartbeat's fourth line.
- Run 38: answered by run 39: the z-fighting stops with an RSX wait after each RSX draw (mode 1).
- Melee run 41 (2026-10-29, `build/run41_udp.txt`): the console created the save. Mute City: mode 0 50-55 presents
  a second, the RSX barrier after each draw 25, the PPE wait 15. The user: only the stage model flickers black, not
  the platforms; the RSX barrier mode flickers less. That mode also holds the PPE back (ring waits 55%).
- Race package 2: Select cycles 0 none; an RSX barrier before 1 constant uploads, 2 VP loads, 3 texture binds; 4 a
  PPE wait at each present (the lead only); 5 an RSX barrier after each draw. Host: all modes, 0 dropped draws.
  Package: `.got` 0x46f8, 0 `r2off` stubs.
- Melee run 41, second boot (`build/run41b_udp.txt`): Mr. Game & Watch and Kirby on Flat Zone; the main thread
  stopped (no message) right after Kirby's copied Chef (`GwSpecialN`). Host (G&W against CPU Kirbies): "stack
  smashing detected" in `it_8027CE64` from `it_802C74D8`. Two LP64 faults, fixed under `TARGET_PS3`:
  - `ftKb_Init_800EEB00` copies the word at +4 of G&W's copy data (`x0.ftDynamicBones`, the item colour) into a
    4-byte `GXColor`; natively that field is an 8-byte pointer, so it overran the stack, and the reader had dropped
    the colour. It now reads the serialized word with `ps3_dat_word` (`ps3/dat/lookup.c`, a native object's
    serialized bytes): 0x000000FF.
  - `it_8027CE64` stores the 8-byte `gamewatch.attr` at +0 of the item vars, over `gamewatchchef.x4` (the sausage
    kind) at +4: `gamewatchchef.x0` is pointer-sized now, so `x4` moves behind it. G&W's own Chef had the same fault.
  Host: four 240 s matches with 18-38 Kirby Chef uses each, and 112 scripted Chefs by a human G&W, with no fault.
  Package: `.got` 0x46f8, 0 `r2off` stubs, 19.1 MB in loaded segments.
- Melee run 42 (2026-10-29, `build/run42_udp.txt`): all six modes flicker; the RSX barrier after each draw least.
  The user: the black flicker is rare, on Mute City only, the stage model only. Leads checked: not texture cache
  pressure (Mute City uploads as many textures as Green Greens in run 40); `lbRefract`'s indirect stages (Zelda's
  Farore's Wind) logged once on Mute City, but run 40's flicker had Peach and DK. What is Mute City's own: the
  display list cache evicts there all the time (run 40: 14.8 lists a second; Fountain of Dreams 17.7; Green Greens
  0), and the stage's lists are drawn from it.
- Texture eviction (`backend.c`): a texture the RSX no longer samples (not bound, not bound in this frame) is freed
  with no RSX wait; EFB copies are evicted last; at most 4 versions of the data at one address (`TEX_VERSIONS`).
- Run 43 package: Select cycles 0 as built, 1 no display list cache (new A/B bit 1024), 2 no list cache and no
  batching, 3 an RSX barrier after each draw. L1 writes "pad: mark" to the log. The fifth heartbeat line adds texture
  evictions, RSX waits for them, lookups that found no texture, and TEV setups with indirect stages. Host: 0 dropped
  draws. Package: `.got` 0x4728, 0 `r2off` stubs.
- Melee run 43 (2026-10-29, `build/run43_udp.txt`): Mute City at 59-60 presents a second in mode 0. The user marked
  flickers (L1) in mode 1 (no list cache), mode 3 (RSX barrier per draw) and mode 0: not the list cache, not batching,
  not RSX timing. At the marks: 0 texture evictions, 0 lookups without a texture, 0 indirect TEVs. Also checked: the
  position transform is the same instruction sequence in every vertex program (no depth variance between layers),
  and the current matrix registers (XF 0x18, 0x19) do feed the constants. Host frames 600-616 on Mute City draw the
  stage as expected. Open: what the flicker looks like (whole stage or parts, black or the background).
- Specular lights (the user's lead): Mute City is the only stage seen with specular lights 2 and 3 besides the
  fighters' light 1 (host log of generated programs). GX's attenuation divides by dist.(1, a, a^2); the program's
  RCP(0) times 0 is NaN where GX (Dolphin's SafeDivide) gives 0. `safe_rcp` (`xfgen.c`) raises a divisor of 0 to
  1e-30 (`C_MIDX + 2`'s y), with the same in `xf_ref`; `progtest` 3000 of 3000, `scenetest` PASS. But Mute City's
  specular lights have k = (s/2, 0, 1 - s/2), s 10 to 100 (host log of XF 0x600-0x67F): the divisor stays >= 1, so
  this is not the flicker. Package rebuilt: `.got` 0x4728, 0 `r2off` stubs.
- Run 41: saving (the first boot creates the save; a setting changed, then a second boot keeps it), Poke Balls,
  the C stick (which mode failed), and the race modes (Select) on Green Greens, Mute City and with the XMB open.
- Run 60: the intro movie without a skip for 40 s or more (the sixth heartbeat line at the music stop), then 4 players
  on Fountain of Dreams to the end of the match and 20 s of the results screen, then Mute City, with a few seconds in
  mode 1 (conversion on the PPE). The listener writes `build/run58_udp.txt` (run 60 starts at its "melee: boot").
- Wii U GameCube adapter (057e:0337, 2026-10-29): `ps3/pad/gcadapter_cell.c` over PSL1GHT's usbd (`-lusb`), an
  extra LDD for that VID/PID with its own static 32-bit ops (PSL1GHT's `usbRegisterExtraLdd` passes a stack copy).
  Attach sets the configuration, opens the interrupt pipes, sends 0x13, then reads 37-byte reports. Adapter port N
  is channel N, with the origin taken when the pad appears; DS3 pads keep their channels, or take the first free
  one. Rumble needs the adapter's power plug. Package: `.got` 0x4858, 0 `r2off` stubs.
- Melee run 44 (2026-10-29, `build/run44_udp.txt`) SUCCESS: the GC pad on the adapter plays. The PS3's HID driver
  does not take the adapter: driver registered 0, attach, pipes in 1 out 2, init command result 0.
- Specular lights, checked on the host (2026-10-30, temporary probe build): Mute City, Peach and DK, 7,320 presents
  (about 2 minutes of match). On each draw with a specular channel: 117 million vertices through `xf_ref`, 0 NaN or
  infinite outputs; 14.6 million also through the vertex program on the interpreter, 0 differences (largest 2.7e-6);
  N.H at most 1.0, the attenuation divisor at least 1.00001, the half-angle vector length 1. The constants after
  each partial update against a full `xf_constants` (3.6 million draws): 0 stale rows. So the specular math is not
  the flicker on the host. Not checked anywhere: the RSX's colour 1 (secondary colour) path, which only the specular
  channel uses. Select mode 4 (A/B bit 2048) clears the lights of the specular channels (attenuation on, spot off):
  they give their ambient colour, black in HSD, so no highlights. If the flicker stays in mode 4, specular is out.
- Fountain of Dreams below 60: the star field (`GrdIzumiStar_TopN_joint`, point size 18) is one display list of
  1,852 point draws (24,630 points, VAT 0, 104,096 bytes). `VC_MAX_DRAWS` was 512, so the list ran as commands each
  frame: run 40's 45 lists a second "run as commands" and 83,295 "batched" draws are this list. It was the only list
  the cache refused in host runs of the boot, the menus, Fountain of Dreams and Mute City. Immediate draws there are
  particles only (`psDispSub`, `psDispParticles`, about 6,800 `GXBegin` a second). `VC_MAX_DRAWS` is 4096 now
  (`backend.c`). Host: 0 lists run as commands, GP vertices 262-286 to 89-104 per mille, GP waits for data 126-175 to
  288-382; the cache check (`-DGX_VCACHE_CHECK`) 1.1 million cached draws, 0 differ.
- `tools/ppm2png.py` split the header with `split()`, which also took pixel bytes with whitespace values: the PNG was
  short and invalid. It reads the header with a pattern now.
- Package (2026-10-30): the list limit, Select mode 4. `progtest` 3000 of 3000, `scenetest` PASS; host matches on
  Fountain of Dreams and Mute City (Select pressed 4 times) exit 0. `.got` 0x4858, 0 `r2off` stubs, 19.2 MB in loaded
  segments.
- Melee run 45 (2026-10-30, `build/run45_udp.txt`, Peach against a CPU Ganondorf in Training): Fountain of Dreams
  at 49-52 presents a second in mode 0 (run 40: 42), 0 lists run as commands, about 204,000 GX draws in 57,500 RSX
  draws. The GP is still the limit (waits for data 32-67 per mille): TEV 292 per mille, vertices 173, constants 94,
  textures 79. Mute City per Select mode: 0 and 4 (specular off) 50-52, 1 (no list cache) 33-35, 2 (no cache, no
  batching) 31-33, 3 (RSX barrier after each draw) 25, with the GP waiting for RSX labels 55% of the time.
- The user's video of run 45 (Fountain of Dreams, mode 0, 11 s): the right part of the green slope and the top of
  the right platform turn solid black for one frame, about 10 times in 11 s. They are drawn black: the black band
  covers what lies behind it. The same symptom as Mute City, so neither Mute City nor its specular lights are the
  cause. In those seconds the log has 0 texture uploads, 0 evictions and 0 list conversions: the PPE wrote nothing
  into RSX memory. The user: on Mute City the slowest mode (3, RSX barrier after each draw) flickers least, as in
  runs 41-43. So the fault is between consecutive draws on the RSX.
- Run 46 package: Select cycles 0 as built, 1 an RSX barrier after each draw, 2 only after cached list draws (bit
  4096), 3 only after ring draws (8192), 4 only before fragment program changes (16384), 5 only before vertex format
  changes (32768: a layout change, as the offsets change at nearly every draw), 6 specular lights off (2048). The mode
  that clears the flicker as well as mode 1 names the step. Host: `progtest` 3000 of 3000, `scenetest` PASS, a match
  through all 7 modes exits 0. `.got` 0x4860, 0 `r2off` stubs, 19.2 MB in loaded segments.
- Melee run 46 (2026-10-30, in `build/run45_udp.txt` from line 4351): the user: none of the 7 modes clears the
  flicker, so it is not a hazard between consecutive draws on the RSX, nor the specular lights (mode 6).
- Dolphin had the same bug: issue 7945, "Black or flickering floor in stages in Super Smash Bros. Melee" (D3D,
  2014), a duplicate of 7929, fixed by PR 1689: an EFB copy rendered into a texture still bound for sampling, which
  D3D then unbound, so the floor was drawn without it. In Melee the floors sample EFB copies for the fighters'
  shadows: per fighter, `lbshadow.c` renders it into a 256x256 corner of the EFB (`HSD_ShadowStartRender`: a white
  rectangle, then the fighter in the stage's shadow intensity), copies it as R4 (`HSD_ShadowEndRender`) and
  projects it onto the floor. Run 45's video fits: the black patch is next to Ganondorf, with a straight edge across
  the grass. Host (a temporary probe that drew the shadow passes in software): 469 shadow copies on Fountain of
  Dreams, Peach against Ganondorf, none mostly dark. So the game and the GX side make the shadow maps correctly.
- Run 47 package: Select mode 1 (bit 65536) turns texture units 1-7 off before an EFB copy renders into its
  destination (`copy_pass`; unit 0 holds the EFB), as Dolphin's fix; the next draw binds its textures again. Modes
  2-7 are run 46's modes 1-6. Host: `progtest` 3000 of 3000, `scenetest` PASS, a Fountain of Dreams match in mode 1
  exits 0 and draws the floor and shadows. `.got` 0x4860, 0 `r2off` stubs, 19.2 MB in loaded segments.
- Melee run 47 (2026-10-31, in `build/run45_udp.txt` from line 6265): mode 1 ran about 8 s on Fountain of Dreams and
  4 s on Mute City; the user saw the flicker in it. Run 48 (from line 9635), mode 1 only on Fountain of Dreams: 19
  L1 marks in about 26 s; the user: rarer than before, still there. The marks come a few frames late. In those
  seconds: 0-7 texture hashes, 0 evictions, 0 lookups without a texture, so the floor gets the shadow copy's entry.
- Run 49 package: Select mode 1 (bit 131072, `check_shadow`) waits for the RSX after each 256x256 copy, reads 900
  inside pixels and 128 border pixels of the shadow texture back, and logs the copies with a dark inside (over
  half) or a dark border (over a tenth), and once every 60 presents the counts. As many dark copies as flickers: the
  shadow pass or the copy fails on the RSX. None: the floor samples a good texture wrongly. Modes 2-9 are run 47's
  modes 1-8. Host: `progtest` 3000 of 3000, `scenetest` PASS; drawn shadow copies read back bright. `.got` 0x4898,
  0 `r2off` stubs, 19.2 MB in loaded segments.
- Melee run 49 (2026-10-31, in `build/run45_udp.txt` from line 10894), mode 1 on Fountain of Dreams: about 7,500
  shadow copies read back, all with a white border; 3 dark inside, in 2 frames late in the session (5410, 5445),
  not the flicker's rate. The user still saw the flicker. So the shadow pass and the copy work on the RSX, and the
  floor draws after a finished copy: the black comes from how the floor uses the shadow, or not from the shadow.
- Run 50 package: Select mode 1 (bit 262144, `white_for`) binds a white 16x16 texture with the same sampling in
  place of each 256x256 EFB copy. The flicker stops: the floor samples the shadow wrongly (texture coordinates, the
  projection's divide, the sampler). It stays: the black is not the shadow (the floor's other textures, lighting).
  Modes 2-10 are run 49's modes 1-9. Host: `progtest` 3000 of 3000, `scenetest` PASS, a Fountain of Dreams frame in
  mode 1 draws normally. `.got` 0x48c0, 0 `r2off` stubs, 19.2 MB in loaded segments.
- Melee run 50 (2026-10-31, in `build/run45_udp.txt` from line 14051), mode 1 on Fountain of Dreams: the user: the
  flicker stays. So the black is not the shadow.
- The alpha test: `apply_alpha_test` turned the test off for every two-compare form it did not know. HSD's default
  for translucent materials (`HSD_SetupPEMode`) is alpha > 0 AND alpha > 0, with depth writes and the test before
  them (ZCompLoc 0): on the GameCube clear pixels write no depth, here they did, and later draws behind them were cut
  out, showing what was drawn before (Fountain of Dreams: the black sky). Host, a 90 s Fountain of Dreams match: about
  950,000 two-compare setups, 5% of them with depth writes, all alpha > 0 AND alpha > 0. The other forms seen,
  without depth writes: >= 1 AND <= 255 (alpha > 0), >= 0 AND <= 1 (alpha <= 1), >= 0 AND <= 255 (always).
- Fix (`alpha_const`, `apply_alpha_test`): a compare that 8-bit alpha always passes or fails (>= 0, <= 255, < 0,
  > 255) is a constant, two equal halves are one compare, and XOR or XNOR with a constant half is that compare or
  its inverse (7 - c). Every form above becomes one RSX alpha test; in a host match the "not drawn as on the
  GameCube" line no longer came. Run 51 package: the fix as built; Select mode 1 (bit 524288) is the old alpha test,
  modes 2-11 are run 50's modes 1-10. Host: `progtest` 3000 of 3000, `scenetest` PASS, a Fountain of Dreams frame
  draws the water, the fountain and the effects. `.got` 0x48c0, 0 `r2off` stubs, 19.2 MB in loaded segments.
- Melee run 51 (2026-10-31, in `build/run45_udp.txt` from line 15224): mode 0 (the alpha fix) about 5 s on Fountain
  of Dreams, then mode 1 to the end. The user: the flicker is still there. The fix stays (GX's behaviour).
- Lead: depth precision between two nearly coplanar surfaces. Run 45's video has a straight edge across the grass
  that follows no polygon, as the line where two near planes cross. GX writes depth from z/w; the program gives
  the RSX z + w, a float sum that rounds near the far plane, so two layers GX keeps equal can differ by a step.
- Run 52 package: Select mode 1 (bit 1048576, `before_rsx_draw`, `dev_depth_bias`) puts each RSX draw of a frame 2
  depth steps nearer than the one before (a polygon offset; the count restarts at the display copy), so a later draw
  wins a near tie. Modes 2-12 are run 51's modes 1-11. The software RSX applies the offset too. Host: `progtest`
  3000 of 3000, `scenetest` PASS, a Fountain of Dreams frame in mode 1 draws normally. `.got` 0x48d8, 0 `r2off`
  stubs, 19.2 MB in loaded segments.
- Melee run 52 (2026-10-31, in `build/run45_udp.txt` from line 16500), mode 1 from about 25 s into the Fountain of
  Dreams match: the user: the flicker stays. Depth precision is ruled out.
- Host frames: 87 consecutive frames of Fountain of Dreams (old alpha test) in software, no black floor; too few to
  rule the host in or out.
- Run 53 package: Select mode 1 (bit 2097152, `watch_draw`), the flicker detector. Each cached list draw's setup
  (drawn or dropped, VP and FP entries, the bound textures, zmode, cmode0, the alpha test, cull, viewport and scissor,
  the channel colours, the light colours, the TEV values; not the matrices), keyed by list and its count in the GX
  frame (`gx_frame`, at the display copy). A setup that differs for one frame and then comes back is logged with
  the list's address and the components, and the TEV rows that changed (at most 60 lines in 60 frames); a count
  every 60 frames. Host, Fountain of Dreams, Peach against Ganondorf: 1-19 such changes in 60 frames, all fighter
  colour animations (`PlCo.dat` and `PlGnNr.dat` lists, by their addresses against the "dat: parse" lines), none in
  `GrIz.dat`. Modes 2-13 are run 52's modes 1-12. `.got` 0x4930, 0 `r2off` stubs, 22.6 MB in loaded segments (the
  detector's table, 3.4 MB).
- Melee run 53 (2026-10-31, in `build/run45_udp.txt` from line 18452), mode 1, Peach against Link on Fountain of
  Dreams: 115 one-frame changes, nearly all fighter colour animations (`PlPeNr.dat`, `PlLkBk.dat`); in `GrIz.dat`
  only one event, frame 2272, the light colours of 356 lists at once (probably the pause). So the stage lists'
  setups, less the matrices, do not change for one frame.
- Melee-X (TechProGabe/Melee-X, this decomp on the original Xbox, ideas only): its v3 notes "Fountain of Dreams'
  grass no longer flicker with the wrong textures"; commit 07d33ae: that was z-fighting with a 16-bit depth buffer,
  as Melee's match camera has near 0.1 and far 16384. It writes depth as a linear function of the clip coordinates
  (`row2 = ZMAX * ((vf - vn) * P[2] + vf * P[3])`), with no sum before the divide. Also there: material and
  texture tracks that flip every frame (Brinstar's lamps) flicker on a progressive display, shown as their mean
  (commit 82b2e49); not seen on Fountain of Dreams' stage here (run 53).
- Depth output (`xf_gen`, `apply_state`): the program gave the RSX z + w, a float sum at w's scale, which rounds
  away z's low bits at stage distance (about 2-4 of the 24-bit steps; a step there is about 0.05 units). It gives
  -z now, and the viewport's z scale is -range and its offset far, so the RSX computes GX's far + range * z / w
  with no sum before the divide. `XfKey.old_depth` keeps z + w for an A/B mode. `xf_ref` and `prog_test` follow
  (every other case each way; 3000 of 3000). The vertex program text put an absolute value's swizzle after the
  bars, which cgcomp dropped (`rsxcheck`'s "xf specular" differed since `safe_rcp`): inside now, 19 of 19 match.
- Run 54 package: the new depth output as built; Select mode 1 (bit 4194304) is the old z + w, modes 2-14 are run
  53's modes 1-13. Host: `progtest` 3000 of 3000, `scenetest` PASS, a Fountain of Dreams frame draws in the right
  depth order. `.got` 0x4930, 0 `r2off` stubs, 22.6 MB in loaded segments.
- Melee run 54 (2026-10-31, in `build/run45_udp.txt` from line 19850), Fountain of Dreams: mode 0 (the new depth
  output) about 10 s, mode 1 (the old z + w) about 110 s, then modes 2-10. The user: the flicker is still there.
  So depth precision is not the cause either; the new depth output stays (GX's formula, fewer lost bits).
- Cause found (2026-10-31, code review and host probes, not yet on the console): the flip. The flicker comes where
  the GP is busy at the retrace (GP idle per mille: Fountain of Dreams 24, Mute City 50; Green Greens 253, Big Blue
  211, Final Destination and Flat Zone about 505; the XMB open makes any stage busy), so the VI's present runs
  between GX commands of the next frame. The firmware's flip (`gcmSetFlip`) runs gcm's default state block: per
  RPCS3's flip handler and an RPCS3 log of the real firmware's flip, all 16 texture units off, the vertex arrays and
  masks, surface, viewport, raster state. `gx_rsx_present` cleared only `textures_bound`, so the first draw after it
  bound its own units, and a later draw in the frame that reused a texture on another unit passed `bind_textures`'
  "same and bound" check, sampled a unit that was off (black), until the next EFB copy's `tex_forget`. The victims
  are the fighters' shadow units (1 and 2) on the shadow receivers: HSD's shadow stage is CPREV * TEXC, so the whole
  receiving DObj draws black, next to the fighter. A host probe (the soft device turns all units off at each present):
  on Fountain of Dreams about 2% of mid-frame presents leave stale draws, 18 of 20 of them stage lists (the grass slope
  with the shadows), the forced frames show the slope right of Ganondorf black with straight edges; on Mute City
  only GrMc.dat track lists go stale, the forced frame shows the whole track black, platforms, cars and fighters
  unchanged. It explains every A/B result: barriers and waits leave the skipped bind, `white_for` acts only on a
  bind, the shadow copies were good, the run 53 detector hashed `bound[]` (the backend's belief), and the host's soft
  `dev_present` never touched the units. Unit 0 also held the XFB after each present (`dev_present` binds it).
- Fix: `gx_rsx_present` calls `tex_forget()`, so every unit is bound again at its next use. Host probe with the fix:
  0 stale draws in about 500 mid-frame presents. Other state the flip resets is sent again after a present already
  (`apply_state`, `dev_target`, `use_vp`/`use_fp`, `bind_attribs`, all constants; the cull cache matches, as
  `dev_present` turns culling off itself).
- Also checked this session and ruled out: PSL1GHT call encodings on the draw path (librsx disassembled), the
  command buffer wrap, the vertex ring fences, clipping with depth clamp (the hardware still clips at w > 0), fog
  (Fountain of Dreams has none; Mute City's fog colour is not black), TXP, TEV values as interpolants, stale
  constants, matrix indices, CPU/GP data races on stage data (all per-frame values go through the FIFO by value),
  the triple buffering (the flip handler fires at the executed flip). Fountain of Dreams' 80x60 water reflection copy
  (a multiply on the translucent water disc) was a lead: a black map blacks out only the water below the water line.
- Intro movie tearing (the user: "blocks or tearing"): `lbMthp_8001F67C` decodes the next frame into the one Y/U/V
  plane set on the main thread, while the GP thread may still upload the frame before from it (HSD lets the main
  thread run a frame ahead; an upload is about 20 ms of GP time on the console). Fix in the port only: `thp.c`
  decodes into one of two staging sets (900 KB of `.bss`), and a new port opcode 0x7D (`gp_post_copy`, `gp.c`) has
  the GP thread copy it into the planes in stream order, then free the cached textures there; before a set is
  written again, `gp_wait_copy` waits for the copy that read it. Host: `thptest` frames byte for byte as before
  (MvOpen, MvEndFox, a GmRegend still); a 170 s boot with `-DGX_TCACHE_CHECK` over the movie: no texture drawn with
  changed data (the movie planes were reported before).
- Run 55 package: both fixes as built; Select mode 1 (bit 16777216) is the old present invalidation (the flicker),
  modes 2-14 are run 54's modes 1-13. Host: `progtest` 3000 of 3000, `scenetest` PASS, a 90 s Fountain of Dreams
  match exits 0 with 0 dropped draws. `.got` 0x4980, 0 `r2off` stubs, 23.5 MB in loaded segments.

Where the work stopped (2026-10-31), for the next session:
- The working tree holds all of runs 45-55's changes, not committed: `backend.c`, `rsxdev.h`, `rsxdev_cell.c`,
  `vpgen.c`, `xfgen.c`, `xfgen.h`, `gp.c`, `gp.h`, `thp.c`, `thp_test.c`, `diag.h`, `pad.c`, `prog_test.c`,
  `rsxdev_soft.c`, `ppm2png.py`, this file.
- Kept as built (GX behaviour or speed): `VC_MAX_DRAWS` 4096 (Fountain of Dreams 49-52 presents a second), the
  two-compare alpha test reductions, the -z depth output, the rsxcheck text fix, the `ppm2png.py` header fix.
- A/B modes only (Select, `pad.c`'s table, mode 0 as built): 1 the old present invalidation, 2 old depth (z + w), 3
  the flicker detector, 4 the per-draw depth offset, 5 the old alpha test, 6 white shadows, 7 the shadow copy
  readback, 8 Dolphin's EFB copy fix, 9-13 RSX barriers in one place, 14 specular lights off. The detector's table
  costs 3.4 MB of `.bss`.
- The UDP listener (port 18194) wrote runs 45-54 into `build/run45_udp.txt`; each run starts at its "melee: boot".
- Melee run 55 (2026-10-31) SUCCESS: the user reports both fixed, the black flicker and the intro movie's tearing.

The UDP log goes to `192.168.178.99`, port 18194 (`ps3/os/udplog_cell.c`). A listener: `nc -ul 18194`, or any
UDP listener on that port.

## Next goal: 60 frames a second in matches

Set by the user on 2026-10-28, to start after run 29. Done when a match runs at 60 presents a second on the
console (the heartbeat's fourth line), with no stop. Done: run 61 SUCCESS (2026-11-01), 4-player matches at 60.

The budget: in run 28's Peach and DK match the GP thread took about 21 µs per GX draw (48,000 a second, 4,000 a
frame, 12 presents). 60 presents need about 4 µs per draw at that count, 5 times less GP time. 83% of the draws
have no CP, XF or BP write since the draw before, yet each one rebuilds and hashes the TEV config (32% of GP
time), checks its textures (12%), and makes the XF key and raster state (8%).

The plan, in this order, measured on the console after each step:

1. Dirty tracking: skip the TEV config and hash, the XF key, the raster state and the texture checks when no
   register they read changed since the draw before (estimate: half of the GP time).
2. Batching: merge consecutive draws with the same state and VAT into one RSX draw (about 4,000 RSX draws a
   frame to about 700): fewer labels, cache invalidations and attribute binds, so the RSX does not become the
   limit next. Estimate after steps 1 and 2: 30-50 presents a second.
3. Vertex cache: convert the vertices of static display lists (the models) once into RSX memory and reuse them
   (vertex conversion is 18% now).
4. Smaller costs: convert only the changed constant rows, as word copies (no float stores and loads, a
   load-hit-store each); release the FIFO every few KB instead of two `sync` atomics per command; drop the
   per-draw backend label and draw record (step 3 diagnostics); keep vertex programs in RSX memory instead of
   uploading each change (1,700 a second).
5. Only if still short: vertex conversion and texture decoding on SPEs, one job per frame or ring segment.
   Not per draw (a mailbox round trip each; the ioq3 port measured per-draw SPU jobs as a loss), and not the
   whole GP thread (sequential, branchy, its caches do not fit 256 KB). The main and GP threads share the one
   PPE core, so work moved to an SPE also helps the game logic, which was busy 30% of the time at 12 presents.

Steps 1, 2 and 4 are done (below). Steps 3 and 5 are now part 2 of the plan, at the end of this section.

Step 1, dirty tracking (2026-10-28): `GpState` has three generations (`gp.c`), bumped only when a value
changes: `bp_gen` (BP and the TEV register sets), `xf_gen` (XF registers but the matrix indices 0x18-0x19 and
the projection 0x20-0x26), `cp_gen` (VCD, VAT). A VCD or VAT write with the same value no longer clears the
vertex sizes. `backend.c` keeps the last draw's layout, TEV config and fragment program, TEV values, vertex
program, raster state (the `apply_state` RSX commands and its result), textures and DQ/TEV constant rows while
the generations they read stay. A copy pass, clear or present drops the raster state; a TEV change drops the
textures and constants; a new frame or TLUT load still checks the textures once. The heartbeat's fourth line
adds "with the last draw's state" (`ps3_prof.state_reuses`).
- Host check: a temporary build compared the registers at each memo hit with a snapshot from the last rebuild:
  0 errors in about 26 million hits (Peach and DK on Yoshi's Story, Mario and Fox on Final Destination, items
  at 4, 90 s each; the boot over the movie and a demo, 150 s). 85% of the draws keep the last draw's state.
  `scenetest` PASS; `gx_test` 3 of 3 PASS.
- Normal package rebuilt: `.got` 0x44c8, 0 `r2off` stubs, 17.4 MB in loaded segments. The call trace package
  for run 29 is not rebuilt with this step.
- Melee run 30 (2026-10-28, `build/run30_udp.txt`): Peach and DK on Yoshi's Story. 29 presents a second at
  about 2,150 draws a frame (run 28: 12), 18-20 at about 4,000. 84% of the draws keep the last draw's state. GP
  per mille: draws 778 (vertices 256, textures and program loads 209, constants 116, RSX commands 94, TEV 82).
  The user: the stop of run 28 again, at DK's down B (the log: `SpecialLwStart`, `SpecialLwLoop`, then the main
  thread's disables stop, outside `OSSleepThread`). The GP is idle: `dev_present` run as idle work clears
  `gp_wait`, so the heartbeat says "waits for nothing". Not reproduced on the host, where rumble is a no-op.
- Melee run 31 (2026-10-28, `build/run31_udp.txt`): test A, rumble off before DK's down B. The Rumble menu
  failed at once: assertion `jobj` (`jobj.h` 932) in `mnVibration_CreatePortPanels` (stack named with
  `build/opdsym.py`, the `.opd` map). `mnVibration_Init` writes 0878, 0888 and 0898 as `(&0868)[1..3]`; with no
  named write, GCC folded their reads to NULL (only 0868 is in the ELF). Fix under `TARGET_PS3` in
  `mnvibration.c`: one array, the four names its elements. Package rebuilt (`.got` 0x44c8, 0 `r2off` stubs).
- Melee runs 32 and 33 (2026-10-28): the Rumble menu works. DK's down B stops the main thread as before, against
  Bowser with rumble on (run 32) and against Zelda with rumble off (run 33, `build/run33_udp.txt`): rumble is not
  the cause. Call trace package rebuilt with step 1 and the menu fix (`.got` 0x5488, 0 `r2off` stubs; symbols in
  `build/calltrace_syms.txt`).
- MWCC stack slots (2026-10-28, from the user's hint): `mpLib_80056C54` (`mplib.c`), which DK's down B calls on
  each slap (`ftDk_Init_8010DB3C`), stores its square root through `sqrt_tmp - 4` and `sqrt_tmp - 5`: slots below
  the array that MWCC lays out for the match. Under GCC the store lands on other stack data. Two more sites:
  `ft_0899.c` (`(&sp1C)[-1]`) and `lbcollision.c` (`sqrt_tmp - 1`); `ftcpuattack.c` and `ftCo_0A01.c` already
  keep it under `MUST_MATCH`. Fixed under `TARGET_PS3` (in-bounds slots, same arithmetic). Not proven to be the
  stop. Both packages rebuilt (`.got` 0x44c8 and 0x5488, 0 `r2off` stubs). Run 34: DK's down B.
- Second GameCube-only audit (2026-10-28, at the user's request). GCC 13 over the game with extra warnings
  (`-Warray-bounds=2 -Wstringop-overflow=4 -Wmaybe-uninitialized -Wnull-dereference ...`, `build/linux_audit`):
  580 sites of the relevant kinds (`build/audit_sites.txt`; the mplib store was among them, missed before). Six agents
  checked every site and searched their directories for unflagged patterns (`build/audit_brief.md`; DOL disassembly
  by name with `build/dol_dis.py`). Fixed under `TARGET_PS3`:
  - Statics read as one block or by DOL distance: `mndatadel.c`, `mndiagram.c`, `mndiagram2.c`, `mndiagram3.c`,
    `mnitemsw.c`, `ftkirbyspeciallw.c`, `ftpurinspecialn.c` and `ftkirbyspecialpurin.c` (next table, assert string),
    the JPEG snapshot tables (`hsd_3B34.c`).
  - Stack and object overruns: `itlinkhookshot.c` (`*(&y + 6)`), `gm_1832.c` (`MTXOrtho` into a 3x4 `Mtx`),
    `lb_020A.c` (a 16-byte rotation into a `Vec3`), `grheal.c` (`_94[n + 2]`), `mninfo.c` (an unterminated joint
    name), the JPEG coder's raw offsets past MWCC's 0x118-byte `jmp_buf` (`hsd_3B5C.c`, `hsd_3B34.c`).
  - LP64 views: `hsd_3B33.c` (a pointer bumped as a u32), `gmtou_0.c` (`TmData_80194F30` packed as `TmData`),
    `grbigblue.c` (car lane step), Kirby's bone byte, mushroom scales over a callback, Master/Crazy Hand state vars
    and item pointers, Pikachu's down B thunder pointer, Yoshi's guard, the CPU item check (`motion_id` vs `kind`),
    the Super Scope `fp + 0x2340` views (`ftCo_ItemScopeStart.c`).
  - Register values the DOL relies on: `gricemt.c` (top check), `itmaplib.c` (`coll_index`), `ftCo_Damage.c`
    (`ftCo_8008DA4C` result), `itkyasarinegg.c` (the missing return destroys the egg).
  - Argument order: `efasync.c` 0x42B reads two `va_arg`s in one call; MWCC evaluates them right to left.
  - Build (`Makefile`): `ps3/include/ps3_msl_math.h` forced into the game: MSL's `sqrtf` returns x for x <= 0, libm
    NaN. String literals as CP932 (`-fexec-charset`), as sjiswrap gives the MWCC build: Japanese literals were UTF-8
    (US text too: "Ｌｖ．", "ＨＩＴ"; with JP, two buffer overruns).
  - Open: `mpLib_80056C54` reads `total_dist` unset on a first wall line (the DOL: the callers' f27); `hsd_3A76.c`
    SIS jump operands read 8 bytes where the stream has 4 (opcodes 8/9, if any .dat uses them); `displayfunc.c` zlist
    offsets (dead code); `gmevent.c:1594`, `mnitemsw.c` `fn_80234C24` state 0 (unreachable as far as known).
  - Clean rebuilds: normal package `.got` 0x44c8, call trace 0x5488, 0 `r2off` stubs. Host matches
    (`build/hostmatch_audit.txt`, items at 4): 46 of 46 run their time: the 13 pairs on Final Destination, Mario
    and Fox on stages 2-31 but 21, Peach and DK, and a human DK doing down B 24 times by pad script.

Steps 2 and 4 (2026-10-28):
- Batching (`backend.c`): a GX draw with no CP, XF or BP write since the draw before (`GpState.writes`), the same
  VAT and the same family (triangles, lines, points) joins the open batch with no state work. Quads, strips and
  fans become triangle indices as the RSX splits them, line strips line indices. The vertices, then the u16
  indices, go into one ring reservation up to the segment end (`dev_ring_reserve`, `dev_ring_commit`). A batch is
  one `rsxDrawIndexArray` (`dev_draw_indexed`; PSL1GHT invalidates the vertex cache in it) and one set of
  attribute binds. EFB copies, finish, present and memory changes send the open batch first. A draw over 65,535
  vertices or 32,768 indices goes alone, as before.
- Constants: `xf_constants` converts only the rows that the written XF memory (`xf_mem_dirty`, a bit per 4
  words) and the written registers feed, as word copies, and marks the rows that differ; the backend uploads each
  run of marked rows. Against the old full conversion (`build/cmp/consts_test.c`): 20,000 random write sets, all
  rows equal.
- FIFO: the GP thread releases ring space every 4 KB (`gp_fifo_release_step`) and before it waits, not after
  each command.
- Vertex programs stay in the RSX program slots 0-503 (the present pass loads its own at 504-511). A loaded
  program only sets its start slot again (`rsxLoadVertexProgramBlock` with no instructions). Programs are placed
  one after the other, over the oldest.
- The per-draw backend label and draw record (step 3 diagnostics) are gone, with `gx_rsx_debug_hang`; at a stall
  the heartbeat still logs the commands before GET. The fourth heartbeat line adds batched GX draws and RSX draws.
- Host: `scenetest` PASS with frames bit for bit as before (31 RSX draws instead of 71); `gx_test` 3 of 3 PASS;
  `progtest` PASS; the boot's memory card notice bit for bit as before. Captain Falcon and DK on Final
  Destination: about 80,000 GX draws a second in 15,000 RSX draws (5.3 to 1), about 1,950 VP loads a second at
  57 presents; the match frames look as before. Host matches (`build/hostmatch_step2.txt`, items at 4, 45 s):
  18 of 18 run their time with 0 dropped draws (the 13 pairs on Final Destination, Peach and DK on Yoshi's
  Story, Mario and Fox on 4 other stages).
- Normal package rebuilt: `.got` 0x4480, 0 `r2off` stubs. The call trace package is not rebuilt.
- Melee run 34 (2026-10-28, `build/run34_udp.txt`): matches at 36-47 presents a second (run 30: 18-29), about
  82,000 GX draws in 16,000 RSX draws; no stop in 167 s, 0 dropped draws. The user: some textures flicker
  (surfaces, menus, the movie), and the console froze after the quit (the log reached `exit()`, with the GP
  thread drawing and the RSX busy; in run 33 the GP was idle at the quit). Fixes:
  - `rsxDrawIndexArray` writes the vertex cache invalidate 0x1714 once; `rsxInvalidateVertexCache` writes 0x1710
    and then 0x1714 three times. `dev_draw_indexed` now does the full invalidate first, as `dev_draw` does, so a
    reused ring address cannot give old vertices or indices.
  - `gp_stop` (`gp.c`), before the exit on the XMB quit (`vi.c`) and in `OSResetSystem`: the GP thread sends its
    open batch, waits for the RSX and stops for good (at most 2 s). Host: "gp: stopped, the RSX is idle", exit 0.
  - Package: `.got` 0x44c0, 0 `r2off` stubs.
- Melee run 35 (2026-10-28, `build/run35_udp.txt`, training mode, Kirby against a CPU Zelda on Green Greens,
  Fountain of Dreams and Hyrule Temple): 36-44 presents a second at 74,000-87,000 GX draws (17,000-18,000 RSX
  draws), GP busy 83-85%; 24-28 at about 100,000 draws. The quit logged "gp: stopped, the RSX is idle". The
  user: artifacts on the floor (flickering triangles, spikes, z-fighting), a wrong camera (zoom, position, start),
  all players at one spawn point, and the CPU Zelda transforms while she stands still.
  - Spawns and camera: each stage logged "use dummy CamRange" and "use dummy DeadRange" (also runs 30 and 33,
    never on the host). newlib makes `ssize_t` an int on the PS3, glibc a long; the schema from `datgen` writes
    it as a long. So `UnkStageDat.count` read 0 on the console, no stage position joint was registered
    (`Ground_801C34AC`), and the spawn points, camera range and blast zones fell back to one point and dummies.
    Fix under `TARGET_PS3` in `gr/types.h`: the two `ssize_t` fields (`count`, `pair_count`) are `long`. These are
    the only `ssize_t` fields in the game's structs, and `ssize_t` is the only schema type whose size differs.
    The console code now loads both as 64-bit.
  - Zelda: training mode's CPU "stop" is CPU kind 0, which starts in behavior 1 (`ftCo_800A101C`'s default case,
    `ftCo_0A01.c`). Every 400 frames that behavior sets the transform flag with chance 0.5
    (`ftCo_800ADE48`), and `ftCo_800ABA34` then transforms Zelda or Sheik. The file is Matching, so the GameCube
    does the same. The host shows it (`MELEE_HOST_MATCH=1h,18k0l1/32/0`; `tests/host_start.c` takes `kN` for the
    CPU kind and `lN` for the level). Not changed.
  - Package: `.got` 0x44c0, 0 `r2off` stubs.
- Melee run 36 (2026-10-28, `build/run36_udp.txt`, Green Greens and Great Bay): no "use dummy" line; the user:
  spawns and camera right, the floor artifacts are z-fighting. Matches at 33-37 presents a second, about 81,000
  GX draws in 16,700 RSX draws. GP per mille: draws 842 (vertices 390, textures 235, TEV 148, constants 40,
  state 17); main thread asleep about 45%.
- A/B package for the z-fighting: Select (no GameCube button, `pad.c`) moves through 5 modes in
  `ps3_diag.gx_ab`: 0 as built; 1 no batching (each GX draw alone, non-indexed, as before step 2); 2 a vertex
  program load at each switch; 4 all constants at each update; 7 all three. Host: a match through all modes,
  0 dropped draws; `MELEE_HOST_PAD` takes `SELECT`. Package: `.got` 0x4500, 0 `r2off` stubs.
- Melee run 37 (2026-10-28): the z-fighting stays in the logged modes 1, 2 and 3 (one change off each) and
  goes only in mode 4, all three off (13 presents a second; batching off alone 31, all on 35). So no single change causes it, or it
  depends on the timing between the GP thread and the RSX.
- Second A/B package: bit 8 waits for the RSX after each RSX draw (`dev_finish`). Select cycles 6 modes: 0 all
  on; 8 all on with the wait (a race test); 6, 5 and 3, one change on alone (batching, VP slots, changed
  constants); 7 all off. Host: all modes, 0 dropped draws. Package: `.got` 0x4500, 0 `r2off` stubs.

### Part 2 of the plan

Set by the user on 2026-10-28, after run 36: all four steps, in this order, each measured on the console. The
done condition of this goal stays: a match at 60 presents a second, with no stop.

The budget at run 36 (Green Greens, 33-37 presents a second, 81,000 GX draws and 16,700 RSX draws a second): GP
per mille: draws 842, of which vertex conversion 390, texture checks 235 (no uploads in a match), TEV 148,
constants 40, state 17. The main thread sleeps about 45% of the time. Estimate after steps 1 and 2: twice the GP
headroom, near 60 presents; then the game logic on the main thread is the next limit.

1. Display list vertex cache: convert the vertices of a display list once, keep them where the RSX reads them,
   and draw later calls of that list from the cache. Key: the list's address and size, VCD and VAT, the array
   bases and strides it reads, the default matrix indices. Invalidation: the game calls `DCFlushRange` or
   `DCStoreRange` (`os/os.c`, now only a `sync`) after it writes vertex data, so a flushed range drops the
   entries that read from it.
2. Texture checks: a hash table by address and format, instead of two scans of the 512-entry cache per lookup.
   A texture is hashed again only when a `DCFlushRange` or `DCStoreRange` covered it or a TLUT load changed its
   palette, with a word-wise hash instead of the byte-wise FNV.
3. TEV setup: find the fragment program by a word compare of the TEV registers it reads, instead of a full
   `TevConfig` rebuild and its byte-wise hash at each BP change.
4. SPUs, if still short of 60: vertex conversion and texture decoding as SPU jobs, one per batch or ring
   segment, with an RSX label wait before the draws that use them. Indexed attributes need gather DMA (each GX
   attribute has its own index). Not per draw: a mailbox round trip each, and the ioq3 port measured per-draw
   SPU jobs as a loss.

Step 1, display list vertex cache (2026-10-28):
- Memory writes (`os/mem.c`): `ps3_mem_written` gives each write a number and keeps the last one per 4 KB page of
  GameCube physical space (512 KB table). Callers: `DCFlushRange`, `DCFlushRangeNoSync`, `DCStoreRange`,
  `DCInvalidateRange` (`os.c`), each DVD read (`dvd.c`), ARAM to main memory copies (`sdk_audio.c`), each `.dat`
  pool block (`pool.c`) and each `HSD_ArchiveParse` buffer (`lookup.c`). `ps3_mem_stamp` gives the GP thread a
  number up to which every write is complete. The game calls `GXInvalidateVtxCache` each frame (`gmscene.c`), so
  that is no signal.
- The cache (`backend.c`, `call_list`, a new `GpBackend` hook that `run_list` calls first): a list of draws and
  NOPs only, one VAT and one family, at most 65,535 vertices and 1 MB, is converted once into a 16 MB ring in RSX
  local memory (vertices, then u16 indices) and then drawn from there: one indexed RSX draw per call, with the
  same state work as a draw that opens a batch. Key: list address and size, VCD, the VAT, the bases and strides
  of the arrays it reads, the default matrix indices when the layout reads them. An entry goes stale when a write
  after its stamp touches the list or the array bytes it read (index range per array). Other lists are kept as
  "run as commands". Evictions wait for the RSX only when the entry was drawn in the current frame.
  `DEV_LOCAL` on an attribute or index offset selects local memory (`rsxdev.h`). A/B bit 1 turns the cache off.
- The heartbeat's fourth line adds lists drawn from the cache, converted, run as commands, and evicted.
- Host check (`EXTRA_CFLAGS=-DGX_VCACHE_CHECK`: each cached draw converts its list again and compares the bytes):
  0 differences in about 2.2 million cached draws (Peach and DK on Yoshi's Story, Fox and Mr. Game & Watch and
  Captain Falcon and DK on Final Destination, Mario and Fox on Fountain of Dreams, items at 4, 60 s each) and
  700,000 in a 150 s
  boot (movie, demo, menus). No evictions. A drawn match frame looks as expected. `scenetest`, `gx_test`,
  `progtest`, `audiotest`, `intr_test` PASS.
- Host profile (qemu, so shares only): vertices from 303-503 to 41-87 per mille in four matches; GP time per GX
  draw 40-70% lower. Each list is now one RSX draw: lists with no register write between them were one batch
  before, so the RSX draws rise (Peach and DK: 12,700 RSX draws for 67,000 GX draws before, 15,300 for 49,000
  now).
- Normal package rebuilt: `.got` 0x45c0, 0 `r2off` stubs, about 19.0 MB in loaded segments (the page table and
  the 4,096 entries).
- Melee run 39 (2026-10-28, `build/run39_udp.txt`): Zelda and Peach on Green Greens, then Peach and DK on Yoshi's
  Story; the quit logged "gp: stopped, the RSX is idle". Select was pressed twice in the first match (23:14:05 mode
  1, an RSX wait per draw: 16 presents a second; 23:14:17 mode 2, VP loads at each switch and all constants), so
  only Green Greens before 23:14:05 is mode 0:
  - Green Greens, mode 0: 46-48 presents a second at about 112,000 GX draws (about 2,350 a frame) in 38,000 RSX
    draws (run 36: 33-37 at 81,000). About 22,000 lists a second from the cache, 0 converted, 0 evicted. GP per
    mille: draws 720 (textures 335, TEV 164, vertices 124, constants 60, state 25); waits for data about 90; main
    asleep about 30%. The user saw the z-fighting there again.
  - Yoshi's Story, mode 2: 34-38 presents a second at about 75,000 GX draws (2,150 a frame; run 30 in mode 0: 29),
    constants 460 per mille (all rows each update). No z-fighting seen. No mode 0 numbers for this stage.
  - Texture checks are now the largest GP share: step 2.
  - The user: on Green Greens the z-fighting stopped at the first Select press (mode 1, an RSX wait after each RSX
    draw, batching, VP slots and changed constants all on). So it is a race between the GP thread and the RSX: some
    data the PPE writes or a command the RSX runs out of order, not the programs or constants as such.

Step 2, texture checks (2026-10-28, `backend.c`):
- The texture cache has chains by address (`tex_bucket`, 1,024), for the EFB copies too: no more scans of the 512
  entries. An entry keeps the `ps3_mem_stamp` its data was hashed at, and is hashed again only after a write to
  its pages; a new hash finds an entry with that hash at that address again. TLUT hashes are kept per TMEM
  offset until the next TLUT load. Hashes go by words (`hash_words`).
- A GX map keeps its lookup while its registers, the TLUT loads and the memory stamp stay; the per-frame check is
  gone. Each free of cached textures drops all lookups and bindings (`tex_forget`); a lookup that freed textures
  binds the earlier units again.
- Not marked as writes, and need not be: CPU stores into heap memory that held a texture (new objects there). A
  texture drawn again must have come through a DVD read, an ARAM copy, a `.dat` parse or a cache flush. The movie
  planes stay freed by the in-stream "memory changed" command (`thp.c`).
- Host check (`-DGX_TCACHE_CHECK`: textures bind at each draw, each one hashed against its entry): 0 textures drawn
  with changed data in 4 matches of 60 s; in the 150 s boot only the movie planes (640x480 Y, 320x240 U and V),
  which the decoder rewrites before their "memory changed" command reaches the GP, as with the per-frame hash
  before. The vertex check still 0 differences.
- Host profile: textures from 49-125 to 24-55 per mille in the same 4 matches, about half per GX draw; 0-4 texture
  hashes a second in a match.
- The heartbeat: OSReport keeps 511 characters, so the cache counters are a fifth line now (lists from the cache,
  converted, run as commands, evicted; texture hashes). Run 39's fourth lines were cut there.
- Normal package rebuilt: `.got` 0x45f8, 0 `r2off` stubs, 19.0 MB in loaded segments.
- Melee run 40 (2026-10-29, `build/run40_udp.txt`, mode 0, Peach and DK, 2-minute matches): Green Greens 60.0
  presents a second (59-60) at about 142,000 GX draws in 42,000 RSX draws, GP draws 460 per mille (TEV 174,
  vertices 137, constants 63, textures 45), waits for data 271; Fountain of Dreams 42 (34-47) at 171,000 GX
  draws, vertices 372 per mille (draws the cache does not take), GP never waits; Mute City 57 (43-60) at 140,000 GX
  draws in 54,000 RSX draws. 0-7 texture hashes a second. The user: no flicker on most stages; flicker and corrupt
  textures while the XMB is open; a black flicker on Mute City.

Step 3, TEV setup (2026-10-31, `backend.c`, `gp.c`):
- A TEV key (`tev_key`) holds the register words `build_tev` reads, less the TEV values: genmode's stage and
  indirect counts, the texgen count, the fog type, the projection bit of each stage's texcoord, the swap tables, each
  stage's colour and alpha words, the order and konst selection halves of the used stages. A table of 1,024 keys
  (`tev_keys`, linear probes, cleared with the program cache) gives the fragment program and the value mask by a word
  compare; `build_tev`, the 688-byte config hash and `fp_get` run only for a new key.
- A config with values in its code (no free interpolant: 5-8 stages, 1-3 texgens, konst colours, registers and fog,
  seen on Final Destination with items) keeps a marker entry, and its key adds the words of those values.
- The TEV values (K0-K3, the PREV and REG0-REG2 start values, fog) are converted again only when their register
  words change; the constants go up again only then. The textures are checked again when the program changes.
- `GpState.tev_gen` changes only with the BP and XF registers the TEV reads (`tev_bp`: 0x00, 0x10-0x1F, 0x28-0x2F,
  0xC0-0xE7, 0xEE-0xF2, 0xF6-0xFD; XF 0x3F-0x47), so a blend, depth, scissor or texture write no longer starts a TEV
  setup.
- The heartbeat's fifth line adds TEV setups and builds a second.
- Host check (`EXTRA_CFLAGS=-DGX_TEV_CHECK`: each draw setup builds the config as before and compares the program,
  the mask and the value rows): 0 differences in about 13.7 million draw setups (Fountain of Dreams, Mute City,
  Yoshi's Story, Final Destination twice, Big Blue, Pokemon Stadium, items at 4, 90 s each; the 150 s boot). Builds:
  about 90-340 a match, the number of fragment programs. No key table clear.
- Host profile (qemu, so shares only; Peach against Ganondorf on Fountain of Dreams, 100 s alone, the old backend
  built in `build/linux_old`): TEV from 195 to 55 per mille, GP draws from 514 to 371 per mille; GP draw time per GX
  draw 37% lower, TEV time per GX draw 75% lower; TEV setups from 33,000 to 24,000 a second.
- `scenetest` PASS with frames identical to the committed ones, `progtest` 3000 of 3000, `gxtest` 3 of 3 PASS.
- Run 56 package: `.got` 0x49b0, 0 `r2off` stubs, 23.8 MB in loaded segments (the key table, 312 KB).
- Melee run 56 (2026-10-31, `build/run56_udp.txt`, from heartbeat 176: the listener started after the boot), mode 0,
  Peach and Marth on Fountain of Dreams: 92 s of match at 59-60 presents a second (mean 59.9; run 45: 49-52), about
  247,000 GX draws in 68,600 RSX draws a second, 0 dropped. GP per mille: draws 589 (TEV 66, vertices 226, constants
  117, textures 95), waits for data 74; main asleep 183. About 32,000 TEV setups a second, 0 builds. At heartbeat 268
  the match ended into the character select with no stop.
- Run 56, Mute City (from heartbeat 275, the same boot), Peach and Marth: 68 s of match at 58-60 presents a second
  (mean 59.7, 3 s at 58; run 45: 50-52), about 150,000 GX draws in 64,700 RSX draws a second, 0 dropped. GP per
  mille: draws 554 (TEV 55, vertices 228, constants 102, textures 89), waits for data 104. The main thread is asleep
  only 65 per mille (0 in 9 of 76 s): the game logic is now the closer limit there. About 21,800 TEV setups a
  second, 25 builds in all.
- Run 56, four players on Fountain of Dreams (Falco, Marth, Ness, Jigglypuff; heartbeats 425-484): 44-57 presents a
  second, mean 48.2; about 227,600 GX draws (about 4,700 a frame; two players: 4,100) in 67,000 RSX draws a second,
  0 dropped, no stop. GP per mille: draws 600 (TEV 64, XF 20, state 55, vertices 244, constants 121, textures 93),
  EFB copies 2, waits for data 81, for RSX labels 35; the other 280 are in no measured phase (command decode, display
  list calls, register writes, or time the thread did not run). Main asleep 95. Lists: 34,000 a second from the
  cache, 0 run as commands.
  Both threads are busy and share the PPE core: with two players the GP drew 247,000 GX draws a second.
- Run 56, four players on Mute City (the same fighters; heartbeats 577-635): 41-56 presents a second, mean 51.8;
  about 150,000 GX draws (2,900 a frame) in 59,600 RSX draws a second, 0 dropped, no stop. GP per mille: draws 531
  (TEV 51, XF 24, state 45, vertices 222, constants 103, textures 83), waits for data 136, for RSX labels 35. Main
  asleep 25 (often 1-20): here the game logic on the main thread is the limit, not the GP. After each match, probably
  the results screen with the four fighters (`GmRst*` loads): about 220,000 GX draws a second at 42-55 presents.
  The quit from the XMB logged "gp: stopped, the RSX is idle" and `melee: exit`.
- Run 56: the user reports SUCCESS (2026-10-31), and asks for Part 2 step 4: the vertex conversion on the SPUs.
- Host profile before step 4 (2026-10-31, a qemu plugin that counts instructions per translation block and thread;
  Falco, Marth, Ness and Jigglypuff on Fountain of Dreams, 100 s of match). GP thread, share of its instructions:
  `run_command` 35.2% (78 M commands, 118 M XF words in LOAD_XF loops) and `xf_write` 7.3%, so command decode about
  45%; `draw_setup` 15.5%; `vtx_convert` 8.8% (with `put_indices` and `batch_add` 9.6%); `xf_constants` and
  `row_words` 7.3%; cached list draws (`call_list`, `bind_attribs`, `dev_attrib`, memory stamps) about 10%. The host
  build has the stack protector, the PS3 build does not. The console's "vertices" phase holds both the conversion
  and the cached list draws; its 280 per mille in no phase fit the command decode.
- The user chose: the command decode first, measured on the console, then the SPU vertex conversion.

Command decode (2026-10-31, `gp.c`):
- Each command loaded `state.writes`, `bp_mask`, the generations and `stats.commands` right after the command before
  stored them (a load-hit-store on the PPE: `ppu-objdump` of `run_command`), and each XF word went through its own
  `take` and an `xf_write` call.
- `run_regs` runs the BP, XF, CP and NOP commands that are whole in the bytes at hand (ring or display list) in one
  pass, up to another command, a BP write with an effect (`bp_has_effect`: PE done, tokens, EFB copy, TLUT load,
  texture invalidate) or 4 KB (the release step). The counters (`Gens`) stay in registers over the pass: no
  per-command stack load or store in the `ppu-gcc` 7.2 code. The other commands take the old path.
- `xf_load` writes the words of one LOAD_XF or LOAD_INDX in one loop, with the XF memory dirty bits gathered per
  word; `writes` counts commands, not words (it is only compared for equality). `bp_dirty` had no reader: removed.
- Host profile (the same 4-player match): GP instructions per GX draw from 2,611 to 1,903 (-27%); the decode
  (`run_regs`, `run_command`) from about 1,200 to 480 a draw. The load-hit-stores it removes do not show under qemu.
- `gxtest` 3 of 3 PASS (GP registers against the SDK's shadow), `scenetest` PASS with frames identical; host matches
  (8, 60 s, items at 4, 4 players on Fountain of Dreams among them) exit 0 with 0 dropped draws; the TEV check build
  0 differences in 5.9 million draw setups.
- Run 57 package: `.got` 0x49b0, 0 `r2off` stubs, 23.8 MB in loaded segments.
- Melee run 57 (2026-10-31, in `build/run56_udp.txt` from line 17109), 4 players on Fountain of Dreams (Mr. Game &
  Watch, Bowser, Peach, Yoshi; not run 56's fighters), heartbeats 30-135: 44-60 presents a second, mean 52.2 (run 56:
  48.2); about 251,000 GX draws a second (4,800 a frame) in 73,700 RSX draws, 0 dropped. GP per mille: draws 671
  (vertices 282, constants 137, textures 98, TEV 70, state 59, XF 22), waits for data 99, for RSX labels 37, in no
  phase 189 (run 56: 280). GP time per GX draw 3.6 µs (run 56: 4.0); outside the draw phases 0.75 µs (1.23). Main
  asleep 25 per mille (run 56: 95): the main thread is now the closer limit there too.
- Run 57, the same 4 fighters on Mute City (heartbeats 190-250): 42-58 presents a second, mean 50.9 (run 56: 51.8);
  about 147,600 GX draws (2,900 a frame) in 59,800 RSX draws a second, 0 dropped, no stop. GP per mille: draws 563,
  waits for data 228 (run 56: 136), in no phase 166; main asleep 33, under 20 in 40 of 61 s. The GP has more spare
  time and the frame rate stays: Mute City is limited by the main thread. The results screen after Fountain of
  Dreams (4 fighters, about 248,000 GX draws a second) ran at 47 presents with the main thread asleep 0.
- The user did not classify run 57, and asked for Part 2 step 4 as planned: the vertex conversion on an SPU.

Part 2 step 4, vertex conversion on an SPU (2026-10-31):
- What converts: the FIFO draws (`batch_add`), one GX draw per batch in a match (about 40,000 a second on the
  console, mostly 4-vertex quads), about 1,600 PPE instructions each on the host. Display lists come from the vertex
  cache; its builds and draws over 65,535 vertices stay on the PPE.
- `vtx_step.h`: the steps and the per-step conversion, shared by `vtx.c` and the SPU program.
- `vxjob.c` (GP thread): a 256 KB entry ring in main memory. An entry holds the header (vertex count, the output
  address in the vertex ring, sizes, default matrix indices), the layout's steps and the arrays' addresses and
  strides only when they changed, then a copy of the GX vertices (the FIFO bytes are released before the SPU runs).
  The new head goes out after a release barrier.
- `vxspu.c` (SPU, `vx_spu.elf`, 6.7 KB code, 95 KB buffers; on a thread in the host builds): reads the ring in
  windows of up to 32 KB, gathers indexed attributes with one DMA list per 64 vertices (each window the 16-byte lines
  of the element's bytes, so never into another page), converts, puts whole 16-byte lines, then writes its tail.
- The RSX must not read the vertices first: PUT moves at `rsxdev_cell.c`'s flushes (ring segment switch, finish,
  present, flush) and at the end of each command buffer segment, in the system's context callback. Each flush runs
  `vx_drain` first (`dev_set_flush_hook`), and so does the callback: `ctx->callback` is replaced by a 32-bit
  descriptor (entry, TOC) of `segment_callback`, which drains, then calls the system's through librsx's
  `rsxContextCallback`. In a batch each draw's vertices start 16-byte aligned (`vx_align`), as the SPU writes whole
  lines.
- A wait of over 0.5 s for the SPU logs it and puts the conversion back on the PPE. A/B mode 1 (bit 33554432): the
  conversion on the PPE; the modes after it are run 55's modes 1-14. The heartbeat's fifth line adds the jobs on the
  SPU and on the PPE, and the GP's waits for the SPU.
- Host check (`EXTRA_CFLAGS=-DGX_SPU_CHECK`: each queued draw also converted on the PPE, compared after the drain):
  0 differences in about 2.5 million draws (4 players on Fountain of Dreams, Mute City, Final Destination, the 150 s
  boot). `scenetest` PASS with frames identical (under `-R 0x100000000`; without it the queue is not below 4 GB and
  the conversion stays on the PPE). 8 host matches of 60 s exit 0 with 0 dropped draws; the A/B switch works.
- Host profile (the same 4-player match): GP instructions per GX draw from 1,903 to 1,691 (-11%); `vtx_convert`
  (12%) gone, `vx_submit` 2.9%.
- Run 58 package: `.got` 0x4aa8, 0 `r2off` stubs, 23.8 MB in loaded segments (and the 256 KB ring at run time).
- The UDP listener of runs 56-57 stopped after run 57 (exit 255, no message); a new one writes `build/run58_udp.txt`.
- Melee run 58 (2026-10-31, `build/run58_udp.txt`): no listener ran during the boot and the Fountain of Dreams match
  (the port was free); the log has heartbeats 220-450, a screen with about 35,000 GX draws a second at 60 presents,
  then the quit. It shows "vertex jobs: 0 on the SPU, 13256 on the PPE": run 58's package checked the command buffer
  room against `ctx->end`, but `end` is the end of the current segment, so the room was always under 64 KB and every
  draw stayed on the PPE. Fixed with the segment callback wrapper above; the room check is gone.
- Run 59 package: `.got` 0x4ac8, 0 `r2off` stubs, 23.8 MB in loaded segments. The boot logs the system callback, our
  descriptor and the segment size ("rsxdev: segment callback ... wrapped"). Host: `scenetest` PASS with frames
  identical; host matches exit 0 with 0 dropped draws (the host's soft device has no segments).
- Melee run 59 (2026-10-31, `build/run58_udp.txt` from line 1436): the boot logged "rsxdev: segment callback 10d80504
  wrapped (entry 0043a630, TOC 00b057f0); segment 28668 bytes" and "vx: vertex conversion on an SPU". 4 players on
  Fountain of Dreams (Roy, Fox, Young Link, Ice Climbers: 5 fighters), heartbeats 153-231: 43.6 presents a second,
  about 237,600 GX draws (5,455 a frame), about 35,800 vertex jobs a second on the SPU, 0 on the PPE, the GP waiting
  for the SPU 0.5 per mille; GP draws 668 per mille (vertices 277), waits for data 123; main asleep 28. Mode 1 was not
  tried. The user: the game crashed. The log: 12 s into the results screen (`GmRst.usd` at heartbeat 233), "Memory
  Empty", `OSPanic` at `sislib.c` line 86, back to the XMB.
- The cause, from the code: `HSD_SisLib_Alloc` serves the text objects from one arena that `preloadState` sizes per
  scene (`gm_1A3F.c`: 0xC000 for the results, 0x2400 the character select, 0x4800 the rest). With LP64 its
  `SisBlock` header is 24 bytes (12 on the GameCube), and `HSD_Text` and `SisBuffer` hold more pointers, so the same
  text needs more of the arena; the results screen with four players ran out. The SPU writes only into the vertex
  ring and its queue line.
- Fix (`sislib.c`, `HSD_SisLib_803A6048`, under `TARGET_PS3`): each scene's text arena is twice the size it asks for
  (results 96 KB). Not reproduced on the host: its debug VS match does not end into the results screen.
- The user: run 59's intro music stopped suddenly. The log: during the intro movie the stream refilled once (ARQ posts
  201 to 205 at heartbeat 3), then none for 30 s while the movie went on (30 DVD reads a second, every read and ARQ
  post with its callback). Runs 54 and 57 refilled every 2 s (4 ARQ posts) until the user skipped the movie at 7-8 s;
  no run before 59 played the movie that long. The refill (`HSD_SynthPStreamMasterClockCallback`, each AX frame) needs
  the stream voice's position to move on, AX frames to run, and no block load in flight. The cause is not found: the
  host boot does not reach the movie. Run 60 diagnostic: a sixth heartbeat line, the audio program's command lists,
  AI periods and silent periods a second (`ps3_audio_stats`) and the stream's state (`ps3_synth_stream_state` in
  `synth.c`: load busy, block indices, the voice's flags, state, current, end and loop addresses). Host: 200 command
  lists and AI periods a second, the stream moving.
- Run 60 package: `.got` 0x4ae8, 0 `r2off` stubs, 23.8 MB in loaded segments.
- Melee run 60 (2026-10-31, `build/run58_udp.txt` from line 14500): the intro movie's music played to the end (the
  user: it worked): a block every 2 s, 200 command lists and AI periods a second, no silence. Same code as run 59, so
  run 59's stop is timing-dependent; the next one shows in the `audio:` line. 4 players on Fountain of Dreams (Peach,
  Ice Climbers, Pikachu, DK), heartbeats 68-128: 41-58 presents a second, mean 50.3, about 247,700 GX draws (4,928 a
  frame), about 34,800 vertex jobs a second on the SPU, GP waits for data 144 per mille, main asleep 25. The results
  screen ran from heartbeat 130 to the character select at 139 with no panic (run 59 panicked 12 s in), the four
  fighters at 48-57 presents with the main thread asleep 0-74. Mute City, the same fighters, heartbeats 152-211:
  35-58 presents a second, mean 50.7 (run 57: 50.9, run 56: 51.8); about 158,000 GX draws (3,117 a frame), about
  25,300 vertex jobs a second on the SPU; GP draws 531 per mille, waits for data 265; main asleep 29 (under 20 in 41
  of 60 s). Its results screen from heartbeat 213, the stats page from 225, still up at 240 with no panic: run 59
  panicked 12 s into the results, about where the stats page starts, so the arena fix holds. Mode 1 was not tried.
- Where the frame rate stands with 4 players: Fountain of Dreams about 50, Mute City about 51. On both the main thread
  (the game logic) is the limit; the GP waits for data 14% (Fountain of Dreams) and 27% (Mute City) of the time.
- The user on run 60: "Perfect". The command decode, the SPU vertex conversion, the text arena fix and the audio
  diagnostic line are committed (8bcfa54).

### Stable 60 with 4 players (2026-10-31)

The user: "Let's do our best to get stable 60 FPS." Done when 4-player matches stay at 60 presents a second.
- Host profile, 4 players on Fountain of Dreams (qemu instruction counts; a plugin per translation block and thread,
  and one that names the callers of a function): main thread 15% in the per-motion `.dat` parse and lookups (strcmp
  4.7%: `dat_public` scanned 12,060 schema roots, 9,312 strcmp a lookup; `dat_wrap` 8,532 a parse), GX API 24%, HSD
  render and matrices about 25%. GP: 68% of the FIFO draws and a third of the RSX draws were GX's XF flush primitives
  (8 zero vertices after each BP write, `GXSetMisc(1, 8)` in `gmmain.c`). GP busy 822 per mille at 50 presents is
  about 987 at 60, so both threads needed cuts.
- PPE costs from the PS3 disassembly: `GX_PIPE_PUT` reloaded `__GXPipeWp` after each store (`-fno-strict-aliasing`),
  a load-hit-store per word; newlib mathfp `sinf`/`cosf` about 186 instructions with 6 calls and 3 GPR/FPR moves
  through memory, 6 calls per joint in `HSD_MtxSRT`; `PSMTXConcat`'s stack temp and `memcpy`; `parseFloat` 3 moves
  and an `fdivs`; librsx reloads `ctx->current` per call, 16 attribute calls per RSX draw.
- Five packages, each host-checked and reviewed in its own worktree, then merged:
  - GX front end: one reservation per command (`gx_pipe.h`, `__gx.h`, `GXVert.h`, `WriteMTXCmd`), `gx` as an
    object, no XF flush primitive (`GXSetMisc`), no SU texcoord registers (the GP never reads BP 0x30-0x3F), BG
    written once, no `peCtrl` re-send, one call per TEV stage (`GXSetTevStagePS3`), a 4-way `GXTexObj` memo in
    `HSD_TObjSetup`, no envelope-blend counter. The ring bytes of unchanged commands hash the same (gx_test, scene,
    the new `streamtest`).
  - Math: `PSMTXConcat` without the temp, `HSD_MtxInverseTranspose` and `HSD_MtxScaledAdd` load first,
    `fabsf_bitwise` as fabs; `parseFloat` by tables, `fterm_f` in `HSD_FObj`'s tail padding; `sinf`/`cosf` as an
    exact newlib mathfp replica (`sdk/trig.c`, asm labels in `ps3_trig.h`). Bit-exact (`mathobjs`/`mathtest`; all
    2^32 floats for sin and cos).
  - `.dat`: a name index for `dat_public` (chains in schema order), `_index`/`_command` once per schema, the
    per-parse lines only with `-DPS3_DAT_TRACE`, `OSReport` formats once (`ps3_log_text`), and a cache of parsed
    fighter motions (`dat/motion.c`: 8 MB, keyed by bytes and name, trimmed to 4 MB at a scene change). Nana's main
    motions parse in place: `ftData_80085E50`'s copy branch rewrites her x59C under her live FObjs, which then read
    the new bytes as on the GameCube; that branch keeps x59C's old archive (`ps3_dat_motion_keep`), so x590 stays
    valid (HEAD freed it there).
  - GP: one shadowed attribute write per RSX draw (`dev_attribs`), constant runs by bit scan, `apply_state` skipped
    when its 18 source words are equal, VAT-masked vertex cache keys in a compact array, `xf_constants` rows once with
    a colour table, texture rebinds only when the used units' register words change (a free after `bind_textures`'
    last pass leaves the units unbound), no `isync` in address translation, pointer copies in the XF decode, a vertex
    program memo.
  - Threads: the pad-queue wait sleeps up to 1 ms until the pad alarm has run, instead of spinning (`PROF_MAIN_PAD`);
    `HandleReverb` runs the 3 channels in one loop, output bit-identical (`reverbtest`).
- New A/B modes (appended): 2^26 the attributes bound by a librsx call each (the old path); 2^27 the GP thread at
  priority 1200, below main (the sampler idles then).
- Heartbeat: a main line (logic, render, XFB and pad wait per mille; logic frames, render passes, µs per frame and the
  longest; GP busy per mille), a dat line (parses; motions from the cache, added, parsed in place; cache entries and
  KB), and every 10 s the sampler (`os/mainprof.c`, 250 Hz): phase shares and the busiest GObj callbacks as `.opd`
  addresses (`ppu-nm melee.elf`).
- Host checks, merged tree: scenetest frames identical; gxtest, progtest 3000/3000, audiotest, reverbtest, alarmtest,
  streamtest, tevfusetest PASS; math_test mtx (1,000,000) and fobj 0 mismatches, trig 1.34 G inputs 0 mismatches. A
  check build with all nine checks (attrib, state, tev, vcache, vx, tcache, tobj memo, dat index, dat motion) over 4
  boots and 4-player matches on Fountain of Dreams, Mute City and Ice Climbers dittos: 0 differences, but vx 2 of
  about 900,000 draws in one Fountain of Dreams run (not in 5 more; the reference converts at submit, the SPU reads
  indexed arrays later, so a game write in between differs). 45 host matches exit 0; 8,2/26 never starts (ground.c
  2310, stage param of stkind 26 not found, then a while(true); the same in the Oct 28 logs).
- Host profile after the merge, same match: main thread per rendered frame 8.58 to 5.19 M instructions (-39%), GP per
  rendered frame 8.01 to 6.36 M (-21%); RSX draws per present 1,391 to 939, vertex jobs 681 to 219; `HandleReverb`
  0.55 of its cost. Instruction counts only: the console decides.
- Open: the motion cache fills its 8 MB in about 90 s of a 4-player match (about 42 KB an entry, mostly the reader's
  maps); then about 9% of the motions parse in place. Host only, from before: 13,14,5,12 on Castle panics "Not Found
  Item_Struct" (`item.c` 1997).
- Run 61 package (2026-11-01, not committed): `.got` 0x4d28, 0 `r2off` stubs, 24.6 MB in loaded segments. The
  worktrees of the packages are in `E:\Users\Matteo\Desktop\Repo\melee-wt` (detached, deletable).
- Melee run 61 (2026-11-01, `build/run61_udp.txt`, DS3, mode 0), Fountain of Dreams with Peach, Ganondorf, Bowser and
  the Ice Climbers (5 fighters), heartbeats 41-103: 60 presents every second but 2 at 59 (run 60, 4 players: 41-58,
  mean 50.3). About 290,000 GX draws in 64,000 RSX draws and 14,000 vertex jobs a second. Main thread per frame:
  logic 3.4-4.8 ms, render 8.0-10.7 ms (longest 15.4 ms), and 9-36% of its time in the XFB wait (ahead of the GP and
  the retrace). GP busy 58-88%. Sampler: fighter drawing 31-39% of the main thread (`ftDrawCommon_80080E18`,
  `80080C28`), the stage 12% (`grIzumi_801CD220`, `grDisplay_801C5DB0`), `Fighter_procAnim` 5-8%. The results
  screen after it: 54-60 presents, GP busy 85-97%.
- Run 61, Mute City with the same 5 fighters, heartbeats 116-178: 60 presents every second but 1 at 59 (run 60: 35-58,
  mean 50.7). About 170,000 GX draws in 47,000 RSX draws a second. Main thread per frame: logic 4.0-5.5 ms, render
  7.6-9.5 ms (longest 13.5 ms), 15-40% in the XFB wait. GP busy 51-67%. Sampler: the stage (`grDisplay_801C5DB0`)
  22-24%, fighter drawing (`ftDrawCommon_80080E18`) 21-25%, `Fighter_procAnim` 8%. Its results screen: 54-60.
- Melee run 61: the user reports SUCCESS (2026-11-01). The goal "60 frames a second in matches" is done with 4 players
  (5 fighters). The GC adapter was not used in this run.

### XMB glitch (2026-11-01)

- The user: opening the XMB with the PS button glitches the renderer. Cause (code review, not yet on the console):
  the backend keeps vertex programs loaded in the RSX's transform program slots (`vp_slots`, 0 VP loads a second
  in run 61's matches) and after a present only selects them again. libgcm loads the program at each set, so the
  system need not keep the slots: while it draws the XMB at the flip (events 0x121 to 0x122), its own vertex program
  can overwrite the loaded ones. The constants already go up again after each present.
- Fix: `ps3_sys_overlay` (`sys_cell.c`, the drawing begin and end events) reaches the backend through the VI's
  retrace (`gx_rsx_set_overlay`); while it is set, and for 30 presents after, each present forgets the loaded
  programs (`vp_forget`), so they load again at their next use. The heartbeat's VP loads rise while the XMB is open.

Other findings of 2026-10-29:
- C stick: the right stick is the C stick (`pad.c`). Host check (`MELEE_HOST_PAD` now takes `CL CR CU CD`): C stick
  right in a VS match gives Mario's forward smash and back air. Not a fault: the user tried it in Training and
  single player, where `fighter.c` (Matching) gives fighters no C stick input (`gm_IsCurrently1PMode_inline`).
- Poke Ball Pokemon: each ball released kinds 161, 162, 163 in turn. The ball's attributes are read as
  `pokemon_spawn` (`it_279C.c`), but the union member annotated for `It_Kind_M_Ball` was `m_ball`, 8 bytes, so the
  native reader converted 8 bytes: all spawn weights 0 (the first two kinds not picked last win) and the spawn
  scale time 0 (`it_8027AAA0` divides by it). Fix: `pokemon_spawn` is the annotated member, `m_ball` a shared view
  (`itemattrs.h`, annotations only), schema regenerated. Host (`tests/item_trace.c`, an item trace build;
  `MELEE_HOST_MATCH` takes an item mask, `0x400000000` is Poke Balls only): the weights read 30 30 30 25 ...,
  the balls release kinds 170, 182, 186. The other Pokemon read their own annotated members.
- Float to unsigned: MWCC converts a float to a 32-bit unsigned with `__cvt_fp2unsigned` (0 below 0, 0xFFFFFFFF
  from 2^32); GCC goes through 64 bits, so -1.0 gives 0xFFFFFFFF. Wobbuffet's `(u32) sonans.x68` then gave a
  fighter 4294967296 damage ("attack power over 500", `ftcoll.c` 1057). `ps3_mwcc_f2u` (`ps3_msl_math.h`) has
  MWCC's result. Of the 85 lines with an `fctidz` in the game and SDK objects, 72 conversions in 37 files use it
  under `TARGET_PS3` (the GameCube code unchanged in `#else`); the others convert to 64-bit or signed types.
  Host: two 200 s matches with Poke Balls only (Mario, Fox, Peach, DK on Final Destination; Zelda, Yoshi,
  Jigglypuff, Pikachu on Fountain of Dreams) run their time with no assertion, dozens of Pokemon released.
- Memory card (`sdk/card.c`): a Memory Card 251 in slot A, its files as GCI files (Dolphin's export format: the
  64-byte directory entry, then the data) in `/dev_hdd0/game/MELE00001/USRDIR/card` (host: `build/card`). Mount
  loads them; create, write, rename, set status and delete write through to the files; callbacks come as a new
  EXI interrupt. Slot B is empty. Host: the first boot creates `01-GALE-SuperSmashBros0110290334.gci` (11 blocks,
  banner, icon and comment set); the next boot loads it and leaves it unchanged.
- Package with all of the above: `.got` 0x46f8, 0 `r2off` stubs, 19.1 MB in loaded segments.
- Race test package: Select cycles what follows each RSX draw: 0 nothing, 1 an RSX barrier (the RSX waits for its
  own backend), 2 a flush, 3 the PPE waits for the RSX (run 39's mode 1). XMB events are logged
  (`sys: XMB event`).

## Goal: netplay with rollback (2026-11-01)

Set by the user: netplay with rollback as Slippi does, 1v1, two consoles per number. The user's decisions:
- Select opens a number entry; consoles that enter the same number play each other. A GC pad also opens it with Z
  held and D-pad Up (it has no Select). The GX A/B modes on Select are removed, when Select gets this job.
- Pairing: LAN broadcast first, then a small UDP server the user runs on a machine with a public IP (hole punching).
- Flow as Slippi's direct connect: each player picks a character and colour on their own screen, player 1 (the
  first console on the number) picks the stage, rules fixed at 4 stocks, 8 minutes, items off.

Plan: 1 save state and a sync test on one console; 2 the netplay core (UDP link, input delay, prediction, rollback,
time sync, desync check, shared RNG seed, no sound from re-run frames); 3 matchmaking and the netplay menus.

Step 1 (2026-11-01, `ps3/net/rollback.c`, `include/ps3_rollback.h`):
- A state is the game's statics, grouped by `rollback_data.ld` and `rollback_bss.ld` (INCLUDEd in `melee.ld` and the
  generated `host.ld`) into `__rb_*` ranges, the OS heap table and the HSD heap up to its high-water mark (`PsHeap.top`,
  the end of what the heap ever wrote). Outside the ranges: state that threads change (synth, AX, VI, devcom, rumble,
  card, `lbarq`, the pad queue `gmMain_8046B108` and `HSD_PadLibData`, the AUX work areas in `lbaudio_ax.c`),
  timings (`perf.o`) and HSD's shadows of GX state (`state.o`, `tev.c`'s value caches, `pobj.c`'s `prev_vtxdesc*`),
  and `grAnime_8049EE40` (a `jmp_buf`). Pointer caches into the heap stay in the state; a restore ends with
  `HSD_StateInvalidate(-1)`. A restore writes only differing 4 KB pages (marked with `ps3_mem_written`), keeps queued
  alarms and `pobj.c`'s shape blend buffers, and zeros the heap from the saved top (a session zeros it from its top).
- Host survey (`MELEE_HOST_RB=survey`): in a match only the HSD heap, the heap table and the statics change in the
  arena and statics; the HSD heap uses about 4 MB of 42 MB. A state is 5.0-8.2 MB.
- The scene loop runs each logic frame through `ps3_rb_frame` (`gm_RunLogicFrame`, `gmscene.c`); `fn_8016E730`
  starts a session, the loop's exit ends it. `HSD_PadRenewMasterStatus` keeps each frame's pad entry; a re-run frame
  gets it back and leaves the queue alone. In a session `.dat` pool frees wait for its end, the motion cache always
  adds, and Nana's in-place parses are kept by buffer, bytes and name (`motion.c`).
- Sync test (`MELEE_HOST_RB=sync:D:K` on the host, `RB_FLAGS=-DPS3_RB_SYNC=D` on the console): each frame is saved
  before and after it runs, then the last D frames run again from the oldest state; the first re-run frame must match
  all memory, each one the gameplay values (fighter motion, position, velocity, facing, animation frame, percent,
  hitlag, shield, stocks, the RNG seed). The heartbeat's rollback line gives the counts and times.
- Render dependencies found and fixed (logic read state that only the render sets, so frames run again without a
  render diverged; in a session the logic now sets it after each frame, `gm_RunLogicFrame`):
  - The off-screen state (`ftLib_UpdateScreenVisibility`: `x221F_b0`, `x2188`), which off-screen damage reads
    (`fighter.c` 1597): `ftLib_UpdateScreenVisibilityAll`, and the render no longer writes it in a session.
  - The magnifier's `is_offscreen` and `edge`: `ifMagnify_UpdateState`, render writes off in a session.
  - The camera CObj: the render copies the logic camera into it (`Camera_8002A4AC`) and computes its viewing matrix
    (`HSD_CObjSetCurrent`); the replica does both first. Pokemon Stadium's screen reads the matrix
    (`grStadium_801D32D0`).
  - The particle lists: `particleSort` (`psdisp.c`) re-links them, and the particle update walks them with RNG calls
    per particle: `psSortParticles` sorts after each frame (the sort is stable, so the display's own changes nothing).
  - Not a render dependency: the re-run skipped `lbAudioAx_80027DF8`, whose frame timers are state and whose sounds
    take the RNG; it runs in re-runs too, so re-run frames play their sounds again (step 2 must silence them).
- Host diagnostics: `tests/rand_trace.c` (EXTRA_SRCS with `--wrap` on the RNG calls: a re-run frame whose RNG callers
  differ logs both lists), `MELEE_HOST_SEED` (`host_start.c`: the boot seeds the RNG from the clock),
  `MELEE_HOST_RB_RENDERDIFF`, `MELEE_HOST_RB_HASHAT`, `MELEE_HOST_RB_FIRST` (in `rollback.c`).
- Host results after the fixes, depth 2 every frame, going on from the re-run states: 10 matches of 3,221-5,412
  frames with 0 gameplay differences (stkinds 2, 3 (Pokemon Stadium, with Ice Climbers and without), 8, 12, 18, 24,
  31, 32; items on and off). The exact compare still differs in sound handles (synth state is outside the state).
- Netplay notes for step 2: both consoles need one RNG seed at the match start (`gmmain.c:156` seeds from the clock);
  a re-run of N frames costs N logic frames (3.4-5.5 ms each with 4 players in run 61).
- Melee run 62 (2026-11-01, `build/run62_udp.txt`, Battlefield): "4 states of 4734 KB need 19 MB, 29 MB free": the
  session refused to start (16 MB kept free), the match ran normally. The full-copy states do not fit.
- The store since: a shadow of the latest state plus an undo log (the old contents of the 4 KB chunks each save found
  changed, 2,048 chunks), in one block from the high end of the HSD heap (`ps_heap_alloc_high`; the game uses 4-8 MB
  of the 42 MB heap in a match), no system memory. The heap gets 8 MB to grow; past that the session ends. A save scans
  the statics, the table and the heap up to its top; a restore puts the shadow back, then the undo log newest first.
  Host: a save 80-210 chunks (at most 452), a restore 160-630; the store 21-25 MB. The sync test's exact check is a
  chunk hash against the last run from the same saved state (a re-run re-saves); gameplay as before. 4 host matches
  at depth 1 and 2: 0 gameplay differences; the exact check only flags the fighters' sound handles.
- Run 63 package: `RB_FLAGS=-DPS3_RB_SYNC=2`, `.got` 0x5198, 0 `r2off` stubs, 24.7 MB in loaded segments.
- Melee run 63 (2026-11-01, `build/run62_udp.txt` from line 2094, Battlefield, 2 players): 1,243 frames, 2,482 frames
  run again, 0 gameplay differences, no crash; the exact check flagged only the sound handle chunks. Store 19 MB of the
  35 MB HSD heap, 29 MB system memory free. Costs: a save about 10.5 ms, a restore 16.5 ms, a hash 13 ms, a re-run
  logic frame 0.2-1.1 ms: about 50 ms a logic frame. The PPE compares at about 0.6 GB/s (newlib `memcmp`, misses).
  The same package on Pokemon Stadium (from line 3902): store 19.2 MB, 28 MB free, 697 frames, 0 gameplay differences.
- The user (2026-11-01): the save and restore move to an SPU; no delay-based netplay at all, rollback only.
- SPU program `net/rbspu.c` (`rb_shared.h`, `rbspu_io.h`; `rbspu_cell.c` on the SPU, `rbjob_cell.c` starts it,
  `tests/rbspu_host.c` runs it on a host thread): jobs RB_SAVE (compare each chunk with its shadow, the old shadow
  into the next undo slot, the chunk into the shadow), RB_RESTORE (changed chunks get their shadow back, then a list of
  the oldest saved contents per chunk goes into chunks and shadow) and RB_HASH (`rb_hash`, four 32-bit lanes), 8
  chunks of DMA in flight. The main thread waits at low SMT priority. Chunks and shadow slots are 128-byte aligned
  (`rollback_*.ld` align the ranges). The PPE path stays as the fallback. Host: 4 matches at depth 1 and 2, 0
  gameplay differences on the SPU path. SPU ELF 4 KB code, 69 KB buffers.
- Run 64 package: `RB_FLAGS=-DPS3_RB_SYNC=2`, `.got` 0x5218, 0 `r2off` stubs, 24.7 MB in loaded segments.
- Melee run 64 (2026-11-01, `build/run62_udp.txt` from line 5023): Battlefield, 7,438 frames, 0 gameplay
  differences, logic at 60 a second (180 logic frames a second with the re-runs, about 40 presents): a save about
  1.0 ms (run 63: 10.5), a restore 2.0 ms (16.5), 80-130 chunks a save. Pokemon Stadium: about 6,560 frames, then
  assertion `archive` (`lbarchive.c` 129) from `grStadium_801D4548` → `grAnime_801C65B0`: the transformation loads
  its stage file with an asynchronous read (`startLoad`, `lbFile_80016580`, callback `fn_801D4220`), whose callback
  ran outside the frames; a restore took the stage back to loading after it, with the buffer's older bytes. Also a
  netplay desync: each console's load time would start the transformation on another frame.
- Fix: in a session `lbFile_80016580` waits for the read in the frame that starts it (as `waitForDisc`), and the
  callback runs inside that wait. Only Pokemon Stadium loads that way in a match.
- Host, with the fix (`build/rb/ps3t_13.txt`, `ps3t_15.txt`, `sync:2:1`, 420 s): Pokemon Stadium, 5,630 and 6,340
  frames, the first transformation (`GrPs1.dat`) at about frame 3,800 in a session, 0 gameplay differences in all
  417 heartbeats, no assertion. The "memory" counts (0-12 chunks) are render writes, as in runs 63 and 64.
- Run 65 package: `RB_FLAGS=-DPS3_RB_SYNC=2`, `.got` 0x5218, 0 `r2off` stubs, same segments as run 64.
- Melee run 65 (2026-11-01, `build/run65_udp.txt`): Battlefield 7,584 frames and a 390-frame match, 0 gameplay
  differences. Pokemon Stadium: the same assertion at frame 6,139. Presents 20-36 a second (logic 2.1 ms a frame, run
  3 times, plus saves, restores and hashes: about 870 ms of each second).
- The run 64 diagnosis was wrong. Both crashes are at `grpstadium.c` 2164 (`grStadium_801D4548` + 0x8f8), the return
  to the normal stage: `grAnime_801C65B0((void*) xCC)` reads the load buffer as an `UnkArchiveStruct` and clears its
  start (24 bytes on LP64; `unk8` at +16 is the header's 0x5b/0x58/0x47/0x5b, so the first run frees nothing). The
  buffer is the preload of entry 0x7D5 in lbHeap heap 4, outside the store: a re-run of that frame reads the cleared
  start, `unk0` 0 and `unk8` 0, and calls `lbArchive_80016EFC(NULL)`.
- Fix: `ps3_rb_cover(p, n)` adds one buffer outside the HSD heap to the store until the scene ends (range "buffer",
  before the heap); `grStadium` init covers the 0x50000 preload buffer. The synchronous load stays: netplay needs the
  load to end on the same frame on both consoles.
- Run 66 package: `.got` 0x5228, 0 `r2off` stubs.
- Host (`build/rb/ps3c_13.txt`, `ps3c_15.txt`, 900 s): 9,628 and 10,927 frames, 0 gameplay differences; seed 15 went
  GrPs1, back to normal, GrPs4. The host's debug VS skips the stage select (`mnstagesel.c` 909 sets the preload), so
  there `xCC` comes from `HSD_MemAlloc` in the HSD heap ("buffer 0 KB"): the cover runs only on the console.
- Melee run 66 (2026-11-01, `build/rb/run66.txt`): Pokemon Stadium through the stage select, "buffer 320 KB", 7,565
  frames to the match's end, 0 gameplay differences in 127 heartbeats, no assertion. Extra DVD reads (over the music
  stream's 5 each 1.75 s) at about frames 1,900 and 3,900: two transformations, so a return to normal between them.
- Step 1 done.

Step 2 (2026-11-01). The user's decisions: the test is over the internet with a friend's PS3; the matchmaker (8-digit
code, LAN broadcast and a UDP server) comes into this step; player 1 sets up each match in the normal VS menus (port 2
as a CPU for the friend's fighter), player 2 starts any VS match and takes player 1's setup; input delay is a setting,
default 2 frames (Slippi's), with rollback for the rest.
- `net/netplay.c`, `include/ps3_netplay.h`: one UDP socket (port 51000, `net/link.h`: `net/link_cell.c` on PSL1GHT,
  `tests/link_host.c` on the host), a thread that receives and resends, the main thread sends each frame's inputs.
- The PS3 menu (`pad/menu.c`, 400x256 now) has INPUT DELAY (0-7 frames, `netplay_delay` in `options.txt`) and
  NETPLAY (the code, the pairing's state, a status line).
- Pairing: the PS3 menu's NETPLAY row takes 8 digits. The console broadcasts the code on the LAN and
  registers it with the matchmaker (`tools/matchmaker.py`, UDP 51001, Python 3 without packages): the first console
  on a code is player 1. The matchmaker answers both with the other's public and LAN address; both send HELLOs to
  both until each hears the other (hole punching). A different EBOOT (a hash of `.text`) refuses the pairing. Paired
  consoles keep alive every 500 ms; 15 s of silence ends the pairing. The matchmaker's address: `NET_SERVER=host:port`
  at build, or `netplay_server=` in `USRDIR/options.txt`.
- Handshake at the end of `gmVsMelee_EnterVs` and `gmVsMelee_EnterSuddenDeath` (`gmvsmelee.c`): player 1 sends its
  start data (rules up to their callbacks, the six players), ports 1 and 2 as humans, an RNG seed and its tap jump and
  C-stick options; player 2 replaces its own, then its preload cache, sound banks and announcer follow it (as the CSS
  and SSS exits do), and answers with its options. Callbacks that differ (another VS mode) refuse it. B or 120 s ends
  the wait; the match is then local. `fn_8016E730` sets the seed and each port's options (`ps3_net_match_begin`).
- Session (`rollback.c`, netplay mode): each tick takes the inputs that arrived; if a frame ran with a remote input
  other than the real one, the state goes back to it and the frames run again; then the tick's frame runs with the
  remote input predicted (the last one held). The local input of a tick is for frame + delay. Frames whose remote
  input is known are not saved (`save` keeps only the newest state after such a gap). At most 7 frames run on
  predictions; past that, and at frame 0 until the other console's packets come, ticks run no frame (their pad
  entries dropped, `HSD_PadDropQueueEntry`). Time sync: each console sends its frame advantage; the one ahead by more
  than a frame skips a tick, at most every 30 frames. A scene end in a frame with a predicted input waits for the real
  one; if it was wrong, the frames run again and the match may go on.
- Desync check: every 30th frame's gameplay values (RNG seed, stocks, fighters' motion, position, velocity, percent,
  shield...) hashed when final, sent and compared. A desync logs both consoles' values (the other console sends its
  own), so one log shows both sides.
- Re-run sounds: a frame's re-run takes the voices its last run started for the same sounds, and starts no new ones;
  the voices it does not take again stop (`HSD_AudioSFXStartParam`, `axdriver.c`). The sync test uses it too.
- Rumble: the other console's port does not rumble, ours rumbles the local pad (`PADControlMotor`).
- The heartbeat's netplay line: frame, confirmed frame, the other console's frame, round trip, advantages,
  rollbacks, stalls, packets, desyncs.
- Host test (`tools/hostnet.sh`): two host instances in one container, the matchmaker on the PC
  (`host.docker.internal`), a generated pad script each; `MELEE_HOST_NET_LAG=ms:jitter:loss` delays or drops what
  arrives (`tests/link_host.c`), `MELEE_HOST_NET_DELAY` sets the input delay, `MELEE_HOST_NET_NOLAN` turns the LAN
  broadcast off. Docker's proxy shows the matchmaker 127.0.0.1, so the instances connect by their LAN addresses.
- Host results (Fox and Peach on Battlefield, items off):
  - Run 1: pairing by LAN broadcast; one side became paired from a HELLO without answering it, the other timed out.
    Fixed: every HELLO gets an answer.
  - Run 2 (matchmaker, delay 2, no lag): 8,446 frames, 0 rollbacks (each remote input came before its frame),
    0 desyncs.
  - Run 3 (delay 0, 40 ms lag, 10 ms jitter, 2% loss): 7,650 frames, 279 and 261 rollbacks, 0 desyncs, no fault.
    qemu runs it at about 30 frames a second: a save there takes about 30 ms (1 ms on the console, run 64).
  - Run 4 (delay 1, 30 ms lag, 10 ms jitter, 2% loss): 283 and 252 rollbacks, 144 and 145 checks compared, 0 desyncs.
  - Not run on the host: player 2's path through the VS menus (the host starts the debug VS mode), a desync (the
    log and the exchange of values), real routers (hole punching), the console's sockets.
- Matchmaker: a 1 s socket timeout, as Python on Windows does not stop a blocked `recvfrom` for Ctrl+C or a signal.
- Package: `.got` 0x5908, 0 `r2off` stubs, about 24.8 MB in loaded segments (`build/ppu/game/melee.pkg`), with the
  user's icon (`ps3/ICON0.PNG`, 320x176). It has no matchmaker address (`NET_SERVER` empty): without
  `netplay_server=` in `options.txt`, only the LAN broadcast pairs.
- `tools/hostnet.sh` ends the matchmaker by its Windows PID: devkitPro's MSYS2 Python does not get Git Bash's signals.
- Risks known before the hardware test: Corneria's stage logic reads whether a sound still plays
  (`Ground_801C54DC`, `grcorneria.c` 2572), which the audio thread decides; player 2's heap history differs from
  player 1's (its own menus), so a read of memory never written would differ; the desync check names the frame.

Hardware test (the user and a friend, over the internet):
1. The matchmaker runs where both consoles reach UDP 51001: `python tools/matchmaker.py` on a machine with a public
   IP, or on a PC whose router forwards UDP 51001 to it (and whose firewall lets Python receive on it).
2. Both consoles install the same package, built with `NET_SERVER=<address>` (or `netplay_server=<address>` in
   `USRDIR/options.txt` on each).
3. Both: Select, NETPLAY, A, the same 8 digits, A. The row shows PLAYER 1 or PLAYER 2.
4. Player 1: VS mode, own fighter in port 1, the friend's in port 2 (as a CPU), the stage. Player 2: VS mode, any
   fighter and any CPU, any stage. Both start.
5. The UDP log of the user's console has the netplay lines; a desync logs both consoles' values there.

- Netplay run 1 (2026-11-01, internet, `build/run65_udp.txt`, the matchmaker on the user's PC, 94.35.119.59): the
  user's console (player 1, behind a router that keeps ports) and the friend's (84.221.251.196, a router that gives
  each destination its own port). Found on the way:
  - Hole punching failed: each console sent to the port the matchmaker saw, which the friend's router keeps for the
    matchmaker only. Fix on the user's side: the router forwards UDP 51000 to the PS3; the friend's HELLOs come
    in from their own port, and the console answers to it.
  - The matchmaker kept a waiting console 120 s: the friend paired with a console that had quit. Now 5 s without a
    registration (consoles register each second). A second search from the same address paired with itself: now it
    replaces the first.
  - The friend's console left a pairing once, most likely by A on the NETPLAY row (A stops a pairing).
  - One handshake waited 120 s for player 2's answer while the pair stayed up: player 2 did not start in time.
  - Then match 1: Bowser and Peach on Fountain of Dreams, input delay 2. 7,705 frames (2 min 8 s) to the match's
    end and the results screen; round trip 36 ms on average (83 at most); 154 rollbacks (188 frames run again,
    6 at most, 17 in one second at most); 4 time sync stalls, 0 waits; 0 desyncs in 257 checks. Saves 1.4 ms,
    restores 1.5 ms, a frame run again 2.4 ms; logic about 1 ms and render about 4 ms a frame, 60 frames a second.
  - Hardware test SUCCESS (2026-11-01, the user). Step 2 done. The user's setup: `netplay_server=94.35.119.59:51001`
    in `options.txt` on both consoles; the router forwards UDP 51001 to the PC (192.168.178.99, with a firewall rule)
    and UDP 51000 to the PS3.

## Goal: the PS3 menu (2026-11-01)

Set by the user, before netplay step 2: Select opens a menu of extra features; netplay becomes an entry of it later.
The user's decisions: tap jump and the C-stick mode are per port; the menu opens only outside matches.

Done; hardware test SUCCESS (2026-11-01, the user):
- `pad/menu.c`, `include/ps3_options.h`: Select on any DS3 pad, or Up with Z held on any pad, opens it; B, Start or
  Select close it. While open, and until the closing buttons are up, the game gets the pads at rest. A match start
  (`fn_8016E730`, so demos too) closes it; `ps3_menu_match` in `gmvs.c` and `gmscene.c`. The GX A/B modes on Select
  are gone (`ps3_diag.gx_ab` stays 0).
- Options saved on close to `USRDIR/options.txt` (host: `build/options.txt`): `widescreen`, `tap_jump`,
  `cstick_tilts` (one digit per port). They are outside the rollback ranges and fixed during a match, so netplay can
  exchange them at the match start.
- Picture: 400x184 A8R8G8B8 drawn with `HSD_DebugFontAtlas` on the GP thread (`gx_rsx_set_screen`); `dev_present`
  copies it to local memory (two copies used in turn) and blends it at a whole scale (2 at 720p). A host check
  (scratch, not in the repo) drew it and ran open, move, toggle, close, the in-match refusal and the GC combo.
- Widescreen: render only. `xf_constants` scales x of perspective projections by 0.75 when the viewport is the EFB's
  full width (XF 0x1A is 320.0; renders to textures stay), and the present shows the XFB at 16:9 (letterboxed on a
  4:3 TV). The logic camera keeps `cm_803BCB64.aspect`, so off-screen checks stay at 4:3. Orthographic draws (HUD)
  stretch by 4/3.
- Tap jump off (`ftCo_TapJumpOn`, humans only): ground jump (`ftCo_Jump_GetInput`, `fn_800CAF78`), double jump
  (`ft_did_jump`), multi-jump rise (`ftCo_JumpAerialF1.c`), wall jump (`ftCo_800C1E0C`), Peach's float hold.
- C-stick tilts (`ftCo_CStickTilt`, humans only): where a smash check sees the C-stick (`AttackS4`, `Hi4`, `Lw4`
  `CheckInput`), the tilt check runs with the C-stick as the control stick and A pressed (forward: facing the
  C-stick first). Dash (`ftCo_AttackS4_8008C114`) and jump squat (`CheckInputNoD0`) give nothing. With an item, the
  C-stick gives the tilt check's light throw, not a smash throw. Aerials and other C-stick uses stay.
- Builds: `./build.sh -j8 game`: `.got` 0x5330, 0 `r2off` stubs, about 25.0 MB in loaded segments; host game
  build, `scenetest` PASS, `progtest` 3000 of 3000.

## Unlocks, challengers and saves: investigation (2026-11-01)

The user: run the character unlocks, the challenges and the saves in Docker, make sure they cannot stop the game on
the PS3, and list the fixes for another session. Nothing was changed or built: host runs of the existing host build,
and read-only code audits.

How the host runs were made:
- A copy of `build/linux/game/melee_host` as built at 04:42 (HEAD 6a96d37 with the netplay session's uncommitted
  `gmscene.c`, `psdisp.c`, `motion.c`), run in a scratch folder, so `build/card` stayed untouched.
- A GCI save editor (Python, not in the repo): the per-sector `HSD_Encrypt` checksum and scrambling of `crypt.c`.
  `GmSaveData` is logical file 1, one block, mirrored (sectors 1 and 10 of the 11-block file); the nametag banks are
  files 2-8. The VS match count (`save_data.unk_8.x0`, offset 0x8) and `unk_28.x0` (0x28) were set to 49: the next
  VS match gives Jigglypuff's challenge and Brinstar Depths (both at 50).
- The menus by `MELEE_HOST_PAD` (one human, a CPU with a random fighter, a random stage):
  `300:START,520:START,800:DOWN,870:A,1000:A,1300:SL+SD:40,1360:SR:15,1390:SU:16,1420:A,1440:SU:17,1470:SL:5,1490:A,1560:START,1900:START,2000:START`.
  START at 300 skips the intro movie, START at 520 is the title, then VS Mode, Melee. In the character select the
  cursor moves 1.24 units a retrace at full stick (clamp 80): the lower left corner, then right 15 and up 16 is P2's
  HMN/CPU button (A: P2 becomes a CPU with a random fighter), up 17 and left 5 puts P1's coin on Mario, A, START;
  START at the stage select picks a random stage. The 2-minute match starts at about retrace 2,100 and the results
  load at 9,533 (logic stays 1:1 with the retraces).
- The results screen wants any button (state 2), then START after its 50-frame animation (state 3): a single START
  that comes too early leaves it waiting for good (not a fault). START pairs 60 retraces apart, every 300 from 10400,
  and A every 200 from 10500, pass the results, the challenger notice and the prize screen; in a match a pair pauses
  and resumes.
- `MELEE_HOST_DRAW=1` and `=2` stall this build: the presents stop after 57-79, the main thread waits in
  `VIWaitForRetrace` from `HSD_VICopyXFBAsync`. Without drawing the boot, menus and matches run. So no frame dumps.

Host results:
- The challenge works on the host: VS match (49 to 50), results, the challenger notice (`NtAppro.usd`, the "new
  challenger" music), the challenger match on Pokemon Stadium (Mario against a level 5 Jigglypuff, 1 stock each),
  Mario loses, the prize screen (`IfPrize.usd`, Brinstar Depths), back to the character select. No fault, no stall.
- Saves: the card is written after the results and at the prize screen (the mount logs each time). The GCI then
  holds VS 50, play time 120 s and stage bit 0 (Brinstar Depths), sequence numbers rising. A reboot with that file
  loads it, keeps the values and writes it again. A pause in the challenger match (START) idled 3 minutes with no
  fault.
- Peach's Castle crashes twice, both "Not Found Item_Struct" (`item.c` 1997, `OSPanic`): the attract demo after the
  title (Link, Kirby, Captain Falcon, Bowser; only the boot, no test hook), from `grCastle_801D06CC`; and a VS match
  through the menus, from `grCastle_801D0550` (`it_2E6A_Logic117_DmgReceived`). Both from `Item_8026A8EC` via
  `grMaterial_801C8CDC`. The same as the host-only "13,14,5,12 on Castle" in the open issues: any Castle match can
  hit it. Cause in fix 1.
- "use dummy CamRange ...!" on Pokemon Stadium in every host log with that stage (the challenger match here, and
  `build/hostmatch/6-16_3_4.txt`, `8-2_3_4.txt`): joints 0x94-0x96 are not found, so the camera takes the default
  range (`Ground_801C39C0`). No "dummy DeadRange". Not checked on the GameCube.
- The heartbeat's ARQ posts run 135-169 ahead of the callbacks from the stage select on, and the gap stays: posts
  with no callback or removed ones (`sdk_audio.c` counts only callbacks run), not a stall.

Fixes to do, most severe first:
1. **Peach's Castle switch blocks (crash, seen twice on the host).** `grcastle.c` lays `struct unkCastle`
   (`_pad[0x10C]`, `x10C[5]`, `x134[5]`) over the `Ground` with the GameCube offsets. The callbacks
   `grCastle_801D0550`..`801D0680` and `grCastle_801D06CC`..`801D08AC` (tables `grCs_803B7F28`, `grCs_803B7F3C`) take
   the block item from `arg1->x10C[i]` and set `arg1->x134[i]`. On LP64 the `Ground` header and
   `grCastle_GroundVars10`'s 15 JObj pointers move `u.castle10.x10C` (u32) and `u.castle10.state`, so the callback
   destroys a wrong value. Fix under `TARGET_PS3`: take `Ground*` and use `gp->u.castle10.x10C[i]` (cast to the item
   GObj) and `gp->u.castle10.state[i]`.
2. **Credits (crash expected, from the audit).** `gmstaffroll.c` allocates and clears `staffInfo` with
   `sizeof(struct staffInfo_t)` (0x948: 198 entries of 12 bytes) and `staffInfoSortBuf` with 0x2E68 (198 of 0x3C).
   On LP64 an entry is 24 and 0x48 bytes. `fn_801AAB74` walks all 198 entries each frame, so entries 99-197 lie in
   the sort buffer's block: garbage `HSD_Text*` to the SIS calls, and text pointers written over sort entries. The
   credits run after every Classic, Adventure and All-Star clear (`gm_Mode_GOver_States`). Fix under `TARGET_PS3`:
   `ARRAY_SIZE(gm_803DBFD8) * sizeof(*staffInfo)` and `* sizeof(*staffInfoSortBuf)` for both `HSD_MemAlloc` calls
   (lines 1229-1230) and the `memzero` (line 1334).
3. **`lb_800138EC` has no `return` (`lbspdisplay.c` 652-714).** Declared `HSD_GObj*`, it ends after
   `GObj_SetupGXLinkMax`. `gmregclear.c:1062` keeps the result in `state->x2C`, and `lb_800138D8`/`lb_800138CC`
   write through its user data: the bonus screen after each 1P stage clear. It works only while r3 still holds
   `gobj`. Fix: `return gobj;` under `TARGET_PS3`.
4. **Card file I/O while the interrupt lock is held (hitches, lost pad samples, audio dropouts at each save).**
   HSD's ring (`card.c` `hsd_803AAA48`) and `lbcardnew.c`'s mount, format, delete and rename tasks call the CARD
   functions with interrupts off. `ps3/sdk/card.c` does the file I/O in the call (`save()`: fopen, fseek, fwrite,
   fclose), and each mount reads every GCI in the folder again (`load()`): `executeNextTask` unmounts after each
   operation and `CARDUnmount` frees the files. The same class as the DVD reads that kept the lock for a whole
   file. Fix in `card.c`: keep the files across `CARDUnmount` (writes already go to disk), and run `save()`,
   `remove()` and `rename()` on a worker thread that raises `PS3_INTR_EXI` when done, as `dvd.c` does (HSD sets its
   busy flag before it restores interrupts, and the lbcardnew tasks count under the lock, so a later callback is
   safe).
5. **`finish()` drops a 17th pending callback but returns READY (`card.c` 83-86).** Then `hsd_804D799C` stays 1 or
   `tasks_remaining` never reaches 0, and `lb_8001CDB4` (each mode change) spins for good. The game keeps one
   operation in flight, so unlikely. Fix: check the queue before the I/O and return `CARD_RESULT_BUSY`, or `OSPanic`.
6. **Smaller `card.c` items.** `CARDRenameAsync` keeps the new path when `rename()` fails, so later writes get
   IOERROR: report IOERROR and keep the old name. `CARDFreeBlocks` wraps below 0 with more than 251 blocks in the
   folder: clamp at 0. `load()` takes every `.gci` in the folder, whatever its name.
7. **Rollback and saves (only if a save runs inside a session).** The card statics are outside the saved state, but
   `lbCardNew_AllocWorkArea`'s `work_area` and `lib_area` (HSD's `sector_buf`) come from the HSD heap, which a
   restore writes back; a restore between two ring commands would write a wrong block. Saves finish at each mode
   change (`gm_1A3F.c`, `lb_8001CDB4`), but Training, Home-Run and Multi-Man call `lbCardGame_SaveChanges` inside the
   mode. Fix: allocate both from the OS heap, or hold saves while a session runs.
8. **Pokemon Stadium's camera range (low).** Compare with Dolphin whether the GameCube also logs "use dummy
   CamRange" there; if not, find why joints 0x94-0x96 are not registered (`Ground_801C2D24`).
9. **Host tool.** `MELEE_HOST_DRAW=1`/`2` stall (above): frame dumps do not work in this build.

Checked and safe (audits): the VS unlock checks (`gmVsMelee_ExitResults`, `gm_16F1.c`, `gm_1601.c`), the challenger
data and the Approach scene (`gm_1736.c`, `gm_1BFA.c`, `gmapproach.c`: `ScNtcApproach_scene_data` is a typed root,
`HSD_AObjSetRate` takes a NULL aobj), the prize list (`un_804A1F48_t` matches ifprize's view, 16 bytes) and
`gm_8049E558`, `GmSaveData` and the nametag banks (no pointers, so the GCI stays GameCube-compatible, 11 blocks),
`CARDDir` (64 bytes), the busy-waits on card results (each pass calls out, the states escape), the
`tasks_remaining` race (CARD calls are made with interrupts off, so their callbacks run at the caller's restore),
`CARDRead`/`CARDWrite` (dead code, the link has none), the results screen (`gm_8046E1B0` under `TARGET_PS3`), the
`gmregclear.c` bonus values. Right today but fragile: `gmregclear.c`'s `RegClearEv` view over `CameraBlurData` (the
pointer's alignment lands on the right fields). Debug only: `toy.c` `ToyUnkJObjData` reads JObj+0x10 as `child`
(LP64: `next`) in `_Toy_8030B530`, installed only with `_Toy_sbss_804D6EA2` set.

Not run yet (each needs a host hook, so a build):
- Winning the challenge (`gm_UnlockCKind`, the unlock notice, the character select's reveal). A `--wrap` on
  `gm_DecideChallengerCpuLevel` in `host_start.c` that also sets `gmVsMelee_StartData.players[1].cpu_kind = 0` (the
  caller sets it before the call) gives an idle CPU that a pad script of smash attacks can KO.
- The 1P clears (bonus screen, credits, ending movie, trophy, the challenge after them), event unlocks, the trophy
  lottery. A hook that ends Classic as cleared, or enters `GM_GOVER`, would reach fixes 2 and 3 on the host.

## Open issues (2026-10-29)

The user's list of what is left to address. Details of each are in the run entries above.

1. **Stage floors flicker black for one frame, now and then.** Mute City's stage model (runs 40-45) and, in run 45's
   video, Fountain of Dreams' right slope and right platform top: drawn solid black. It happens in every A/B mode
   of runs 41-54 and with no PPE writes into RSX memory (run 45). Ruled out: the display list cache, batching, RSX
   barriers per draw or before constants, VP loads, texture binds or fragment programs, the specular lights (run
   46), texture cache pressure, failed texture lookups, indirect stages, the current matrix registers, the shadow
   maps and their sampling (runs 49, 50, host), a one-frame change in the stage lists' setups less the matrices
   (run 53), and depth precision (runs 52, 54). Fixed on the way, flicker unchanged: two-compare alpha tests (run
   51), the depth output (run 54). Cause found 2026-10-31: the flip turns every texture unit off, and texture
   bindings were kept across a mid-frame present; fixed with `tex_forget()` in `gx_rsx_present`. Run 55 SUCCESS
   (see the entries after run 54), with the intro movie's tearing fixed in the same package.
2. **32-bit and 64-bit faults left in the game code.** The LP64 class: GameCube views of structs that hold
   pointers (a pointer field over a 32-bit field in a union or item/fighter vars, as the G&W Chef's
   `gamewatch.attr` over `gamewatchchef.x4`), `.dat` fields read as another type than the native reader converted
   (as Kirby's G&W colour, now read with `ps3_dat_word`), and pointer-sized stores into 4-byte buffers. The host
   build's `-fstack-protector` caught one ("stack smashing detected"); host matches with the fighter, item and stage
   in question find most of them. Not done from the audits: item findings 5-7, the `ft/` tether views.
3. **GameCube calls and compiler behaviour that may still differ.** Fixed so far: MWCC's float to 32-bit unsigned
   conversion (`ps3_mwcc_f2u`, 72 sites), MSL's `sqrtf`, GCC's hoisted loads in busy-waits, MWCC stack slots,
   argument order, registers the DOL relies on. To look for: other MSL or runtime calls with other results than
   newlib's, more such conversions in code not compiled into the scanned objects, and DOL-offset reads.
4. **Smaller open items.** Fountain of Dreams runs at 49-52 presents a second (run 45; 42 before the cache took its
   star field list). The GP is the limit there, and the TEV setup its largest part (292 per mille): Part 2 step 3
   (TEV lookup) is in the run 56 package. Step 4 (SPUs) is not done. The title ID is still the placeholder `MELE00001`.
5. **Next: what can still crash or hang the game.** The missing GX features and OS thread calls only degrade or
   go unused; these can stop the game:
   - LP64 faults left in the game code (issue 2): item finding 7, the `ft/` tether and chain views.
   - MWCC and GCC differences (issue 3): MSL or runtime calls with other results than newlib's, float conversions
     and DOL-offset reads in code not scanned yet.
   - `mpLib_80056C54` reads `total_dist` unset on a first wall line; `hsd_3A76.c` SIS jump operands (opcodes 8/9)
     read 8 bytes where the stream has 4.
   - Peach's Castle panics "Not Found Item_Struct" (`item.c` 1997): seen in the attract demo and a menu VS match
     on 2026-11-01, cause and fix in "Unlocks, challengers and saves" (fix 1). Host only: 8,2/26 never starts
     (`ground.c` 2310, stage param of stkind 26 not found).
   - The credits, the 1P bonus screen and the card I/O under the interrupt lock: fixes 2-7 of that section.

## Not done for the game

| Part | Size or note |
|---|---|
| GX on RSX | Not drawn yet: indirect textures, TEV compare stages, two-compare alpha tests, logic ops, z textures, depth copies, emboss texgen, constant destination alpha, line width, a texcoord 7 matrix index in the vertex (the backend logs each once). Speed: FIFO draws, and display lists the cache does not take, are de-indexed on the PPE at each draw; textures are hashed again only after a write to them |
| CARD | Saves on the HDD; the stubs report no card in either slot |
| OS threads | `OSSuspendThread` only counts; thread queues (`OSSleepThread`) are the minimal ones in `ps3/os/intr.c` |

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
| SDK shims `ps3/sdk/`, `ps3/pad/`, `ps3/vi/` | `mtx.c`: `PSMTX*`, `PSVEC*` and the C-only `mtx.c` calls, after the SDK's C versions; `mtx44.c`, `OSArena.c`, `PadClamp.c` as they are. `card.c`: no card. `stubs.c`: MCC, FIO. PAD: DS3 (Cross, Square, Circle, Triangle as A, B, X, Y; R1 Z; L2 and R2 the analog L and R, with a click at 230; right stick the C stick), the first four connected of seven ports. VI: the RSX vblank raises the retrace interrupt; a flushed XFB goes to the GP thread (`gp_post_present`), which presents between commands or while it waits |
| Game sources | `debug.c`: `HSD_LogInit` is empty under `TARGET_PS3` (newlib's stdout has no write hook). `hsd_3915.c`: `GX_WG`. Fonts: `tools/dol_inc.py` takes `HSD_DebugFontAtlas` and `HSD_SisLib_FontAtlas` from `orig/GALE01/sys/main.dol`, as `config.yml` names them. `src/MSL/float.c` for `MSL_TrigF_80400770` and `MSL_TrigF_80400774` |
| Final link (2026-10-27) | `make game`: the game, SDK and port objects, PSL1GHT libs, embedded SPU program; `EBOOT.BIN` and `melee.pkg`. Linker script `ps3/melee.ld`, `-fno-common`. `.got` 0x3f90, 0 `r2off` stubs; `.text` 9.8 MB, `.rodata` 2.3 MB, `.data` 0.8 MB, `.bss` 3.4 MB; loaded segments 18.0 MB. Main thread priority 1100, below the port's threads (1001): the game's loop spins between frames. `OSPanic` logs up to 16 return addresses from the back chain |
| `.dat` toolchain `ps3/tools/datgen/` | Docker image `melee-datgen:1` (`Dockerfile`: Debian trixie, clang 19, lld, Rust stable, Aurora e6a6f02, newlib headers). `extract.py` writes `orig/GALE01/files` and `sys/main.dol` from the ISO (the DOL's SHA-1 matches `build.sha1`). `datgen.sh`: the PPC DWARF build, `melee-dat`, `types.bin` (1.9 MB), `native codegen` into `ps3/build/datgen/schema` (163 C files, 22 MB) |
| Native `.dat` readers | `tools/dat-cli/native` and the schema in the EBOOT, with `-mminimal-toc`, allocations in the `.dat` pool (`ps3/dat/redirect.h`). `make dattest` (TOOLCHAIN=linux, qemu-ppc64, the EBOOT's headers and flags): upstream's HSD test passes, with typed dispatch, externs and 27 fighters |

| AXFX effects | The SDK's `axfx/*.c` in the EBOOT: C versions of the asm loops in `reverb_hi`, `reverb_std` and `chorus` (`#ifdef TARGET_PS3`, fused multiply-adds as Gekko's), `long` to `s32` in the AX and AXFX sample types, `AXFXDelayInit`'s missing return. `make audiotest`: each effect on an AUX tone, finite output with the expected tail |
| THP `ps3/sdk/thp.c` | Baseline JPEG in C (Huffman with a 9-bit lookahead, libjpeg's islow IDCT) into GX I8 tiles; replaces the stubs. Host: the intro movie in the game; MvOpen, MvEnd and the stills match a reference decode |

## Open decisions

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
- The SDK's `long` is 32-bit under MWCC. With LP64 it is 64-bit, which broke the AUX buffers that the AXFX
  callbacks read, `__AXOutSBuffer` and the depop sums (`AXSPB.c`). They are `s32` now; under MWCC `s32` is `long`.
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
- A schema type changed only under `TARGET_PS3` breaks the `.dat` reads: the schema comes from the GameCube
  layout and writes native fields by name. Change only runtime structs that way; a `.dat` struct needs an
  annotation or a change in both builds.
- Host profiling: `ppc64-qemu-plugins:1` has qemu 8.2.2 built with plugins (`/opt/qemu-8.2.2/build/qemu-ppc64`).
  A plugin that counts instructions per translation block and thread, with the `.text.<name>` sections of
  `melee_host.map` for the names, splits a run by function and thread.
