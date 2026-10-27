// GX texture decoding. Tile layouts and colour expansion as Dolphin's TextureDecoder_Common.
#include "texdec.h"

static const u8 tile_w[16] = { 8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0 };
static const u8 tile_h[16] = { 8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0 };
// Bytes per tile.
static const u8 tile_bytes[16] = { 32, 32, 32, 32, 32, 32, 64, 0, 32, 32, 32, 0, 0, 0, 32, 0 };

u32 tex_size(u32 fmt, u32 width, u32 height)
{
    u32 tw, th;

    fmt &= 0xF;
    if (tile_w[fmt] == 0) {
        return 0;
    }
    tw = (width + tile_w[fmt] - 1) / tile_w[fmt];
    th = (height + tile_h[fmt] - 1) / tile_h[fmt];
    return tw * th * tile_bytes[fmt];
}

bool tex_is_ci(u32 fmt)
{
    return fmt == TF_C4 || fmt == TF_C8 || fmt == TF_C14X2;
}

static inline u16 be16(const u8* p)
{
    return (u16) (p[0] << 8 | p[1]);
}

static inline u32 argb(u32 a, u32 r, u32 g, u32 b)
{
    return a << 24 | r << 16 | g << 8 | b;
}

static inline u32 x5(u32 v)
{
    return v << 3 | v >> 2;
}

static inline u32 x6(u32 v)
{
    return v << 2 | v >> 4;
}

static inline u32 x3(u32 v)
{
    return v << 5 | v << 2 | v >> 1;
}

static inline u32 rgb565(u32 c)
{
    return argb(0xFF, x5(c >> 11), x6((c >> 5) & 0x3F), x5(c & 0x1F));
}

static inline u32 rgb5a3(u32 c)
{
    if (c & 0x8000) {
        return argb(0xFF, x5((c >> 10) & 0x1F), x5((c >> 5) & 0x1F), x5(c & 0x1F));
    }
    return argb(x3((c >> 12) & 7), ((c >> 8) & 0xF) * 17, ((c >> 4) & 0xF) * 17, (c & 0xF) * 17);
}

static inline u32 ia8(u32 c)
{
    u32 a = c >> 8, i = c & 0xFF;
    return argb(a, i, i, i);
}

static u32 palette(const u8* tlut, u32 tlut_fmt, u32 entries, u32 index)
{
    u32 c;

    if (tlut == NULL || index >= entries) {
        return 0;
    }
    c = be16(tlut + 2 * index);
    return tlut_fmt == TL_IA8 ? ia8(c) : tlut_fmt == TL_RGB565 ? rgb565(c) : rgb5a3(c);
}

static inline u32 blend(u32 a, u32 b)
{
    return (a * 5 + b * 3) >> 3;
}

// One 4x4 CMPR sub-block at (x0, y0).
static void cmpr_block(u32* dst, u32 pitch, const u8* s, u32 x0, u32 y0, u32 width, u32 height)
{
    u32 c0 = be16(s), c1 = be16(s + 2), idx = (u32) s[4] << 24 | (u32) s[5] << 16 | (u32) s[6] << 8 | s[7];
    u32 r0 = x5(c0 >> 11), g0 = x6((c0 >> 5) & 0x3F), b0 = x5(c0 & 0x1F);
    u32 r1 = x5(c1 >> 11), g1 = x6((c1 >> 5) & 0x3F), b1 = x5(c1 & 0x1F);
    u32 pal[4], x, y;

    pal[0] = argb(0xFF, r0, g0, b0);
    pal[1] = argb(0xFF, r1, g1, b1);
    if (c0 > c1) {
        pal[2] = argb(0xFF, blend(r0, r1), blend(g0, g1), blend(b0, b1));
        pal[3] = argb(0xFF, blend(r1, r0), blend(g1, g0), blend(b1, b0));
    } else {
        pal[2] = argb(0xFF, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2);
        pal[3] = argb(0x00, r1, g1, b1);
    }
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            if (x0 + x < width && y0 + y < height) {
                u32* row = (u32*) ((u8*) dst + (y0 + y) * pitch);
                row[x0 + x] = pal[(idx >> (30 - 2 * (y * 4 + x))) & 3];
            }
        }
    }
}

void tex_decode(u32* dst, u32 pitch, const u8* src, u32 fmt, u32 width, u32 height, const u8* tlut, u32 tlut_fmt,
                u32 tlut_entries)
{
    u32 tw = tile_w[fmt & 0xF], th = tile_h[fmt & 0xF], tx, ty, x, y;
    const u8* s = src;

    if (tw == 0) {
        return;
    }
    for (ty = 0; ty < height; ty += th) {
        for (tx = 0; tx < width; tx += tw) {
            if (fmt == TF_CMPR) {
                cmpr_block(dst, pitch, s, tx, ty, width, height);
                cmpr_block(dst, pitch, s + 8, tx + 4, ty, width, height);
                cmpr_block(dst, pitch, s + 16, tx, ty + 4, width, height);
                cmpr_block(dst, pitch, s + 24, tx + 4, ty + 4, width, height);
                s += 32;
                continue;
            }
            for (y = 0; y < th; y++) {
                for (x = 0; x < tw; x++) {
                    u32 i = y * tw + x, c;
                    bool inside = tx + x < width && ty + y < height;

                    switch (fmt) {
                    case TF_I4:
                        c = (s[i / 2] >> ((i & 1) ? 0 : 4)) & 0xF;
                        c *= 17;
                        c = argb(c, c, c, c);
                        break;
                    case TF_I8:
                        c = argb(s[i], s[i], s[i], s[i]);
                        break;
                    case TF_IA4:
                        c = argb((s[i] >> 4) * 17, (s[i] & 0xF) * 17, (s[i] & 0xF) * 17, (s[i] & 0xF) * 17);
                        break;
                    case TF_IA8:
                        c = ia8(be16(s + 2 * i));
                        break;
                    case TF_RGB565:
                        c = rgb565(be16(s + 2 * i));
                        break;
                    case TF_RGB5A3:
                        c = rgb5a3(be16(s + 2 * i));
                        break;
                    case TF_RGBA8:
                        // 32 bytes of AR pairs, then 32 bytes of GB pairs.
                        c = argb(s[2 * i], s[2 * i + 1], s[32 + 2 * i], s[32 + 2 * i + 1]);
                        break;
                    case TF_C4:
                        c = palette(tlut, tlut_fmt, tlut_entries, (s[i / 2] >> ((i & 1) ? 0 : 4)) & 0xF);
                        break;
                    case TF_C8:
                        c = palette(tlut, tlut_fmt, tlut_entries, s[i]);
                        break;
                    case TF_C14X2:
                        c = palette(tlut, tlut_fmt, tlut_entries, be16(s + 2 * i) & 0x3FFF);
                        break;
                    default:
                        c = 0;
                        break;
                    }
                    if (inside) {
                        ((u32*) ((u8*) dst + (ty + y) * pitch))[tx + x] = c;
                    }
                }
            }
            s += tile_bytes[fmt];
        }
    }
}

u32 tex_swizzle(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 offset = 0, bit = 0, i, n = lw > lh ? lw : lh;

    for (i = 0; i < n; i++) {
        if (i < lw) {
            offset |= ((x >> i) & 1) << bit++;
        }
        if (i < lh) {
            offset |= ((y >> i) & 1) << bit++;
        }
    }
    return offset;
}
