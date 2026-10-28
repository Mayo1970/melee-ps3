// Where the GP and main threads spend their time, in PPE time base ticks: the heartbeat logs each phase as a share
// of the second (step 3: the frame rate). Each counter has one writer. Plain C types: PSL1GHT sources include it.
#ifndef PS3_OS_PROF_H
#define PS3_OS_PROF_H

#include <stdint.h>

enum {
    PROF_GP_IDLE,    // GP waits for ring data
    PROF_GP_RSX,     // GP waits for an RSX label (finish, ring segment)
    PROF_GP_FLIP,    // GP waits for a free display buffer
    PROF_DRAW,       // the backend's GX draws, all of them
    PROF_DRAW_TEV,   // TEV config and fragment program lookup
    PROF_DRAW_XF,    // XF key and vertex program lookup
    PROF_DRAW_STATE, // raster state
    PROF_DRAW_VTX,   // vertex conversion into the ring
    PROF_DRAW_CONST, // vertex program constants
    PROF_DRAW_TEX,   // texture lookup, decode and upload
    PROF_COPY,       // EFB copies
    PROF_PRESENT,    // presents, with their flip waits
    PROF_MAIN_SLEEP, // main thread in OSSleepThread (retraces, draw done)
    PROF_MAIN_RING,  // main thread waits for ring space
    PROF_COUNT
};

typedef struct Ps3Prof {
    volatile uint64_t ticks[PROF_COUNT];
    // Draws with no CP, XF or BP write since the draw before, with the same VAT: one RSX draw could take both.
    volatile uint32_t batchable;
    // Draws that converted the changed XF constants.
    volatile uint32_t xf_reloads;
    // GX draws added to the open batch (backend.c), with no state work.
    volatile uint32_t batched;
    // Draws that kept the last draw's TEV config, vertex program and raster state (backend.c memo).
    volatile uint32_t state_reuses;
    volatile uint64_t upload_bytes;
} Ps3Prof;

extern Ps3Prof ps3_prof;

// The Cell can read a time base whose low word is 0 by error (Linux's CELL_TB_BUG): read it again then.
static inline uint64_t ps3_prof_now(void)
{
    uint64_t t;

    do {
        __asm__ volatile("mftb %0" : "=r"(t));
    } while ((uint32_t) t == 0);
    return t;
}

// Adds the time since *t to phase p, and moves *t on.
static inline void ps3_prof_lap(int p, uint64_t* t)
{
    uint64_t now = ps3_prof_now();

    ps3_prof.ticks[p] += now - *t;
    *t = now;
}

#endif
