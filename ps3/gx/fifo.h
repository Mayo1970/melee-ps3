// The GP ring: the write-gather pipe (gx_pipe.h) appends to it on the GX caller's thread, the GP thread
// reads it. Byte positions count the whole stream, so the ring wraps anywhere, as the GameCube FIFO does.
#ifndef PS3_GX_FIFO_H
#define PS3_GX_FIFO_H

#include <stdbool.h>

#include <dolphin/types.h>

// size: a power of two. Allocates the ring on the first call; later calls do nothing.
void gp_fifo_init(u32 size);

// Consumer side, for the GP thread only.
extern const u8* gp_fifo_rp;
extern const u8* gp_fifo_avail;
const u8* gp_fifo_take_slow(u32 n);

// The next n bytes of the stream, contiguous, waiting for the producer when they are not published yet.
// The pointer stays valid until the next take.
static inline const u8* gp_fifo_take(u32 n)
{
    const u8* p = gp_fifo_rp;

    if ((u32) (gp_fifo_avail - p) >= n) {
        gp_fifo_rp = p + n;
        return p;
    }
    return gp_fifo_take_slow(n);
}

// Gives the bytes taken so far back to the producer.
void gp_fifo_release(void);

// The read pointer at the last release. A release costs two full barriers, so the GP thread releases in steps.
#define GP_FIFO_RELEASE_STEP 0x1000
extern const u8* gp_fifo_released;

// Releases once GP_FIFO_RELEASE_STEP bytes are taken; the slow path releases all before it waits.
static inline void gp_fifo_release_step(void)
{
    if ((u32) (gp_fifo_rp - gp_fifo_released) >= GP_FIFO_RELEASE_STEP) {
        gp_fifo_release();
    }
}

// Work for the GP thread while it waits for the stream: gp_fifo_kick runs fn there once, or soon after.
void gp_fifo_set_idle_work(void (*fn)(void));
void gp_fifo_kick(void);

// Producer side: publishes the pipe and waits until the GP thread has run every published byte.
void gp_fifo_sync(void);

#endif
