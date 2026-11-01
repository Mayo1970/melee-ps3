// The write-gather pipe on the PS3, for GXVert.h: each GX store appends to the GP ring (ps3/gx/fifo.c).
#ifndef PS3_GX_PIPE_H
#define PS3_GX_PIPE_H

#include <dolphin/types.h>

extern u8* __GXPipeWp;
extern u8* __GXPipeLimit;

// The most bytes stored after one __GXPipeBegin; fifo.c keeps that much slack past the ring end.
#define GX_PIPE_CMD_MAX 128

// Called when __GXPipeWp reaches __GXPipeLimit: publishes the stores and makes room for GX_PIPE_CMD_MAX bytes.
void __GXPipeMakeRoom(void);
// Publishes the stores to the GP, as a flush of the gather buffer does.
void __GXPipeFlush(void);

// Where the next command goes: store it through the returned pointer, then __GXPipeEnd. Each store may alias
// __GXPipeWp itself, so a local pointer is what spares a reload of it after every store.
static inline u8* __GXPipeBegin(void)
{
    u8* p = __GXPipeWp;

    if (__builtin_expect(p >= __GXPipeLimit, 0)) {
        __GXPipeMakeRoom();
        p = __GXPipeWp;
    }
    return p;
}

#define __GXPipeEnd(p) (__GXPipeWp = (p))

// memcpy, as the stream is byte-packed and most stores are unaligned.
static inline void __GXPipeSt8(u8* p, u8 v) { *p = v; }
static inline void __GXPipeSt16(u8* p, u16 v) { __builtin_memcpy(p, &v, 2); }
static inline void __GXPipeSt32(u8* p, u32 v) { __builtin_memcpy(p, &v, 4); }

#define GX_PIPE_PUT(T, x)                                                                               \
    do {                                                                                                \
        T v_ = (x);                                                                                     \
        u8* p_ = __GXPipeBegin();                                                                       \
        __builtin_memcpy(p_, &v_, sizeof(T));                                                           \
        __GXPipeEnd(p_ + sizeof(T));                                                                    \
    } while (0)

static inline void __GXPipe_u8(u8 x) { GX_PIPE_PUT(u8, x); }
static inline void __GXPipe_s8(s8 x) { GX_PIPE_PUT(s8, x); }
static inline void __GXPipe_u16(u16 x) { GX_PIPE_PUT(u16, x); }
static inline void __GXPipe_s16(s16 x) { GX_PIPE_PUT(s16, x); }
static inline void __GXPipe_u32(u32 x) { GX_PIPE_PUT(u32, x); }
static inline void __GXPipe_s32(s32 x) { GX_PIPE_PUT(s32, x); }
static inline void __GXPipe_u64(u64 x) { GX_PIPE_PUT(u64, x); }
static inline void __GXPipe_s64(s64 x) { GX_PIPE_PUT(s64, x); }
static inline void __GXPipe_f32(f32 x) { GX_PIPE_PUT(f32, x); }
static inline void __GXPipe_f64(f64 x) { GX_PIPE_PUT(f64, x); }

#endif
