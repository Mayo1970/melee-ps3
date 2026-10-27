// The write-gather pipe on the PS3, for GXVert.h: each GX store appends to the GP ring (ps3/gx/fifo.c).
#ifndef PS3_GX_PIPE_H
#define PS3_GX_PIPE_H

#include <dolphin/types.h>

extern u8* __GXPipeWp;
extern u8* __GXPipeLimit;

// Called when __GXPipeWp reaches __GXPipeLimit: publishes the stores and makes room for at least 8 bytes.
void __GXPipeMakeRoom(void);
// Publishes the stores to the GP, as a flush of the gather buffer does.
void __GXPipeFlush(void);

// memcpy, as the stream is byte-packed and most stores are unaligned.
#define GX_PIPE_PUT(T, x)                                                                               \
    do {                                                                                                \
        T v_ = (x);                                                                                     \
        if (__builtin_expect(__GXPipeWp >= __GXPipeLimit, 0)) {                                         \
            __GXPipeMakeRoom();                                                                         \
        }                                                                                               \
        __builtin_memcpy(__GXPipeWp, &v_, sizeof(T));                                                   \
        __GXPipeWp += sizeof(T);                                                                        \
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
