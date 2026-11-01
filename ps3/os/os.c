// The SDK's OS calls that are not threads, alarms, heaps or time: start-up, the arena, the log, reset, console
// settings, CPU contexts and caches.
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dolphin/db.h>
#include <dolphin/os.h>

#include "../dat/pool.h"
#include "../gx/gp.h"
#include "heap.h"
#include "intr.h"
#include "mem.h"
#include "ostime.h"
#include "sys.h"
#include "udplog.h"

// The GameCube's arena is about 20 MB; the game's objects grow with LP64.
#define ARENA_SIZE (48u << 20)
// lv2.ld places .rodata, then .data and .bss, from here up to _end (256 MB aligned).
#define DATA_SEGMENT 0x10000000u

extern char _end[];

// The linker symbols of the GameCube main thread's stack, for db_PrintThreadInfo; that stack is not ours.
unsigned char _stack_addr[8];
unsigned char _stack_end[8];

static u8* arena;

void ps3_os_arena(u8** base, u32* size)
{
    *base = arena;
    *size = ARENA_SIZE;
}

void OSInit(void)
{
    static bool done;

    if (done) {
        return;
    }
    done = true;
    ps3_sys_init();
    ps3_time_init();
    // GX reads textures and display lists from static data too.
    if (!ps3_mem_add_region((void*) (uintptr_t) DATA_SEGMENT, (u32) ((uintptr_t) _end - DATA_SEGMENT))) {
        OSPanic(__FILE__, __LINE__, "no physical range for the static data\n");
    }
    arena = memalign(32, ARENA_SIZE);
    if (arena == NULL || !ps3_mem_add_region(arena, ARENA_SIZE)) {
        OSPanic(__FILE__, __LINE__, "no memory for the arena\n");
    }
    OSSetArenaLo(arena);
    OSSetArenaHi(arena + ARENA_SIZE);
    if (!ps3_dat_pool_init()) {
        OSPanic(__FILE__, __LINE__, "no memory for the .dat pool\n");
    }
    ps3_intr_init();
    OSReport("OSInit: arena %p, %u MB; %u MB free\n", arena, ARENA_SIZE >> 20, ps3_sys_free_mb());
}

void OSReport(char* fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    if (vsnprintf(buf, sizeof(buf), fmt, ap) < 0) {
        buf[0] = '\0';
    }
    va_end(ap);
    // Formatted once; an embedded NUL still ends the line, as it did through ps3_log("%s").
    ps3_log_text(buf, (int) strlen(buf));
}

// The PPC64 back chain: a frame holds its caller's frame at 0, whose word at 16 is the return address into that
// caller. Up to 16 return addresses, for ppu-addr2line on melee.elf.
static void log_stack(void)
{
    const u64* frame = __builtin_frame_address(0);
    char line[256];
    int len = snprintf(line, sizeof(line), "OSPanic stack:");
    int i;

    for (i = 0; i < 16; i++) {
        const u64* next = (const u64*) (uintptr_t) frame[0];

        if (next <= frame || ((uintptr_t) next & 7) != 0) {
            break;
        }
        len += snprintf(line + len, sizeof(line) - (size_t) len, " %08x", (u32) next[2]);
        frame = next;
    }
    ps3_log("%s\n", line);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, msg);
    vsnprintf(buf, sizeof(buf), msg, ap);
    va_end(ap);
    ps3_log("OSPanic %s:%d: %s\n", file, line, buf);
    log_stack();
    ps3_sys_exit();
}

// A reset goes back to the XMB; the PS3 has no reset switch for the game.
unsigned long OSGetResetCode(void)
{
    return 0;
}

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu)
{
    OSReport("OSResetSystem(%d, %u, %d)\n", reset, resetCode, forceMenu);
    gp_stop();
    ps3_sys_exit();
}

BOOL OSGetResetSwitchState(void)
{
    return FALSE;
}

// A retail GameCube: 24 MB, and the game takes no devkit memory from the arena.
u32 OSGetPhysicalMemSize(void)
{
    return 24u << 20;
}

u32 OSGetConsoleSimulatedMemSize(void)
{
    return 24u << 20;
}

static u32 progressive_mode;
static u32 sound_mode = OS_SOUND_MODE_STEREO;

// No progressive scan setting: the RSX output mode is the XMB's.
u32 OSGetProgressiveMode(void)
{
    return progressive_mode;
}

void OSSetProgressiveMode(u32 mode)
{
    progressive_mode = mode;
}

u32 OSGetSoundMode(void)
{
    return sound_mode;
}

void OSSetSoundMode(u32 mode)
{
    sound_mode = mode;
}

// No exceptions reach the game, so error handlers and saved contexts are never used.
static OSContext current_context;
static OSErrorHandler error_handlers[OS_ERROR_MAX];

OSContext* OSGetCurrentContext(void)
{
    return &current_context;
}

u32 OSSaveContext(OSContext* context)
{
    return 0;
}

void OSSaveFPUContext(OSContext* fpuContext) {}

void OSLoadFPUContext(OSContext* fpuContext) {}

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler)
{
    OSErrorHandler old = NULL;

    if (error < OS_ERROR_MAX) {
        old = error_handlers[error];
        error_handlers[error] = handler;
    }
    return old;
}

// The PPE keeps its caches coherent with the SPUs and the RSX; a barrier orders the stores. The game flushes what
// it wrote for the GP, and invalidates what a DMA wrote: either way the GP's caches of that range go.
void DCFlushRange(void* addr, u32 nBytes)
{
    __asm__ volatile("sync" ::: "memory");
    ps3_mem_written(addr, nBytes);
}

void DCFlushRangeNoSync(void* addr, u32 nBytes)
{
    __asm__ volatile("sync" ::: "memory");
    ps3_mem_written(addr, nBytes);
}

void DCStoreRange(void* addr, u32 nBytes)
{
    __asm__ volatile("sync" ::: "memory");
    ps3_mem_written(addr, nBytes);
}

void DCInvalidateRange(void* addr, u32 nBytes)
{
    __asm__ volatile("sync" ::: "memory");
    ps3_mem_written(addr, nBytes);
}

// TRUE keeps the game's crash screen (dberror.c) out: its thread spun on beside OSPanic and wrote to a dead stack frame.
BOOL DBIsDebuggerPresent(void)
{
    return TRUE;
}
