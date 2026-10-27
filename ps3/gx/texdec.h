// GX texture formats to 0xAARRGGBB texels (big-endian words, as the RSX's A8R8G8B8 reads them).
#ifndef PS3_GX_TEXDEC_H
#define PS3_GX_TEXDEC_H

#include <stdbool.h>

#include <dolphin/types.h>

enum {
    TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6, TF_C4 = 8,
    TF_C8 = 9, TF_C14X2 = 10, TF_CMPR = 14
};
enum { TL_IA8 = 0, TL_RGB565 = 1, TL_RGB5A3 = 2 };

// Bytes of one level in GX memory (whole tiles).
u32 tex_size(u32 fmt, u32 width, u32 height);
bool tex_is_ci(u32 fmt);
// One level into dst (pitch bytes per row). tlut: the palette in GX memory for C4, C8 and C14X2.
void tex_decode(u32* dst, u32 pitch, const u8* src, u32 fmt, u32 width, u32 height, const u8* tlut, u32 tlut_fmt,
                u32 tlut_entries);
// The RSX swizzled (Morton) offset of texel (x, y) in a 2^lw x 2^lh texture, in texels.
u32 tex_swizzle(u32 x, u32 y, u32 lw, u32 lh);

#endif
