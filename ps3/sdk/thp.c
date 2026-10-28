// THP video on the PS3: the SDK's THPDec.c is Gekko assembly. Same calls, work area size and output (GX I8 tiles
// for Y, U and V). The frames are baseline JPEG, 4:2:0, with no 0x00 after an 0xFF in the entropy data.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <dolphin/os.h>
#include <dolphin/thp/thp.h>

#include "../gx/gp.h"

#define LOOK_BITS 9

typedef struct Huff {
    // Indexed by the next LOOK_BITS bits: code length << 8 | value, or 0 when the code is longer.
    u16 look[1 << LOOK_BITS];
    // Per code length: the largest code (-1 for none), and the value index minus the code.
    s32 maxcode[17];
    s32 valoff[17];
    u8 vals[256];
} Huff;

typedef struct Dec {
    u16 width, height; // from the frame header
    u16 rows;          // lines to decode, from the caller's header
    u16 restart;       // MCUs between restarts, 0 for none
    u8 tables;         // bit n: huff[n] is read
    u8 qsel[3], dcsel[3], acsel[3];
    const u8* scan;
    u16 qt[4][64]; // natural order
    Huff huff[4];  // id * 2 + class (0 DC, 1 AC), as THPDec.c
} Dec;

typedef struct Bits {
    const u8* p;
    u32 buf; // next bits, left-aligned
    s32 n;
} Bits;

// The work area is 0x4028 bytes plus the frame (THPDec_8032FD40).
_Static_assert(sizeof(Dec) + 16 <= 0x4028, "Dec does not fit the THP work area");

static const u8 natural[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static inline u32 be16(const u8* p)
{
    return (u32) p[0] << 8 | p[1];
}

static Dec* dec_of(void* work)
{
    return (Dec*) (((uintptr_t) work + 15) & ~(uintptr_t) 15);
}

void THPInit(void) {}

// The size and subsampling of a JPEG image, as THPDec.c reads them; 0 when it is not one.
s32 THPDec_8032F8D4(u8* data, THPDec_8032FD40_Data* out)
{
    bool jfif = false;

    memset(out, 0, sizeof(*out));
    if (data[0] != 0xFF || data[1] != 0xD8) {
        return 0;
    }
    data += 2;
    for (;;) {
        u8 m;

        if (*data++ != 0xFF) {
            return 0;
        }
        m = *data++;
        if (m == 0xDA) {
            return 1;
        }
        if (m == 0xC0) {
            const u8* c = data + 8;

            out->_pad = (u16) be16(data + 3);
            out->val1 = (u16) be16(data + 5);
            if (data[7] != 3) {
                return 0;
            }
            if (c[1] == 0x22 && c[4] == 0x11 && c[7] == 0x11) {
                out->val2 = 4;
            } else if (c[1] == 0x21 && c[4] == 0x11 && c[7] == 0x11) {
                out->val2 = 2;
            } else if (c[1] == c[4] && c[1] == c[7]) {
                out->val2 = 1;
            } else {
                return 0;
            }
        } else if (m == 0xE0) {
            if (memcmp(data + 2, "JFIF", 5) != 0) {
                return 0;
            }
            jfif = true;
        }
        data += be16(data);
        if (out->val2 != 0 && jfif) {
            return 1;
        }
    }
}

s32 THPDec_8032FD40(THPDec_8032FD40_Data* data, u16 num)
{
    if (data->val2 != 4) {
        OSReport("ERROR: THP only supports 4:2:0!!!\n");
        return 0;
    }
    return data->val0 + 0x4028 + (data->val1 / 2) * (num / 2) * 2 + data->val1 * num;
}

static u8 read_sof(Dec* d, const u8* p)
{
    int i;

    if (p[0] != 8) {
        return 10;
    }
    d->height = (u16) be16(p + 1);
    d->width = (u16) be16(p + 3);
    if (p[5] != 3) {
        return 12;
    }
    // 4:2:0 only, as THPDec.c.
    if (p[7] != 0x22 || p[10] != 0x11 || p[13] != 0x11) {
        return 12;
    }
    for (i = 0; i < 3; i++) {
        d->qsel[i] = p[8 + 3 * i] & 3;
    }
    return 0;
}

static u8 read_dqt(Dec* d, const u8* p, s32 len)
{
    while (len >= 65) {
        int k;

        if (p[0] >> 4 != 0) {
            return 10;
        }
        for (k = 0; k < 64; k++) {
            d->qt[p[0] & 3][natural[k]] = p[1 + k];
        }
        p += 65;
        len -= 65;
    }
    return 0;
}

static u8 read_dht(Dec* d, const u8* p, s32 len)
{
    while (len >= 17) {
        u32 cls = p[0] >> 4, id = p[0] & 0xF, total = 0, code = 0, k = 0, l, i;
        Huff* h;

        if (cls > 1 || id > 1) {
            return 15;
        }
        h = &d->huff[id * 2 + cls];
        for (i = 0; i < 16; i++) {
            total += p[1 + i];
        }
        if (total > 256 || (s32) (17 + total) > len) {
            return 15;
        }
        memset(h, 0, sizeof(*h));
        memcpy(h->vals, p + 17, total);
        for (l = 1; l <= 16; l++) {
            u32 count = p[l];

            h->valoff[l] = (s32) k - (s32) code;
            h->maxcode[l] = count != 0 ? (s32) (code + count - 1) : -1;
            for (i = 0; i < count; i++, k++, code++) {
                if (l <= LOOK_BITS) {
                    u32 first = code << (LOOK_BITS - l), n = 1u << (LOOK_BITS - l), j;

                    for (j = 0; j < n; j++) {
                        h->look[first + j] = (u16) (l << 8 | h->vals[k]);
                    }
                }
            }
            code <<= 1;
        }
        d->tables |= (u8) (1u << (id * 2 + cls));
        p += 17 + total;
        len -= (s32) (17 + total);
    }
    return 0;
}

static u8 read_sos(Dec* d, const u8* p)
{
    int i;

    if (p[0] != 3) {
        return 12;
    }
    for (i = 0; i < 3; i++) {
        u8 sel = p[2 + 2 * i];

        d->dcsel[i] = sel >> 4;
        d->acsel[i] = sel & 0xF;
        if (d->dcsel[i] > 1 || d->acsel[i] > 1 || !((d->tables >> (d->dcsel[i] * 2)) & 1) ||
            !((d->tables >> (d->acsel[i] * 2 + 1)) & 1))
        {
            return 15;
        }
    }
    return 0;
}

// The headers up to the scan, with THPDec.c's status codes.
static u8 read_headers(Dec* d, const u8* p)
{
    memset(d, 0, offsetof(Dec, huff));
    for (;;) {
        u8 m, st = 0;
        s32 len;

        if (*p++ != 0xFF) {
            return 3;
        }
        while (*p == 0xFF) {
            p++;
        }
        m = *p++;
        if (m == 0xD8) {
            continue;
        }
        len = (s32) be16(p);
        switch (m) {
        case 0xC0:
            st = read_sof(d, p + 2);
            break;
        case 0xC4:
            st = read_dht(d, p + 2, len - 2);
            break;
        case 0xDB:
            st = read_dqt(d, p + 2, len - 2);
            break;
        case 0xDD:
            d->restart = (u16) be16(p + 2);
            break;
        case 0xDA:
            st = read_sos(d, p + 2);
            d->scan = p + len;
            return st;
        default:
            if (m < 0xE0 && m != 0xFE) {
                return 11;
            }
            break;
        }
        if (st != 0) {
            return st;
        }
        p += len;
    }
}

THPFileInfo* THPVideoDecode(void* header, void* status_out, THPFileInfo* work, void* data,
                            THPDec_8032FD40_Data* desc)
{
    const u16* size = header;
    Dec* d = dec_of(work);
    // A byte, as THPDec.c writes it.
    u8* status = status_out;

    *status = read_headers(d, data);
    if (*status != 0 || d->width == 0) {
        return NULL;
    }
    d->rows = size[1];
    return work;
}

static inline void fill(Bits* b)
{
    while (b->n <= 24) {
        b->buf |= (u32) *b->p++ << (24 - b->n);
        b->n += 8;
    }
}

static inline void skip(Bits* b, s32 n)
{
    b->buf <<= n;
    b->n -= n;
}

static inline s32 huff_decode(Bits* b, const Huff* h)
{
    u32 e, l;

    fill(b);
    e = h->look[b->buf >> (32 - LOOK_BITS)];
    if (e != 0) {
        skip(b, (s32) (e >> 8));
        return (s32) (e & 0xFF);
    }
    for (l = LOOK_BITS + 1; l <= 16; l++) {
        s32 code = (s32) (b->buf >> (32 - l));

        if (code <= h->maxcode[l]) {
            skip(b, (s32) l);
            return h->vals[(code + h->valoff[l]) & 0xFF];
        }
    }
    // A corrupt stream: the block goes on as zeros.
    skip(b, 16);
    return 0;
}

// s bits (1-15) as a signed coefficient.
static inline s32 receive(Bits* b, s32 s)
{
    s32 v;

    fill(b);
    v = (s32) (b->buf >> (32 - s));
    skip(b, s);
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

// Returns the zigzag index of the last coefficient set, 0 when only DC is.
static s32 decode_block(Bits* b, const Huff* dc, const Huff* ac, s16* pred, s16 blk[64])
{
    s32 s, k, last = 0;

    memset(blk, 0, 64 * sizeof(s16));
    s = huff_decode(b, dc);
    if (s != 0) {
        *pred = (s16) (*pred + receive(b, s));
    }
    blk[0] = *pred;
    for (k = 1; k < 64; k++) {
        s32 r;

        s = huff_decode(b, ac);
        r = s >> 4;
        s &= 0xF;
        if (s != 0) {
            k += r;
            if (k > 63) {
                break;
            }
            blk[natural[k]] = (s16) receive(b, s);
            last = k;
        } else if (r == 15) {
            k += 15;
        } else {
            break;
        }
    }
    return last;
}

static inline u8 clamp8(s32 v)
{
    return (u32) v <= 255 ? (u8) v : v < 0 ? 0 : 255;
}

// libjpeg's islow IDCT (jidctint.c, Loeffler, Ligtenberg and Moschytz): 13-bit constants, 2 extra bits in pass 1.
#define CONST_BITS 13
#define PASS1_BITS 2
#define DESCALE(x, n) (((x) + (1 << ((n) - 1))) >> (n))
#define FIX_0_298631336 2446
#define FIX_0_390180644 3196
#define FIX_0_541196100 4433
#define FIX_0_765366865 6270
#define FIX_0_899976223 7373
#define FIX_1_175875602 9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172

// One 8-point pass. in[k * step] are the inputs; the outputs come back in o[0-7].
#define IDCT_1D(i0, i1, i2, i3, i4, i5, i6, i7, o)                                                                   \
    do {                                                                                                         \
        s32 z1, z2, z3, z4, z5, t0, t1, t2, t3, t10, t11, t12, t13;                                              \
        z2 = (i2);                                                                                               \
        z3 = (i6);                                                                                               \
        z1 = (z2 + z3) * FIX_0_541196100;                                                                        \
        t2 = z1 - z3 * FIX_1_847759065;                                                                          \
        t3 = z1 + z2 * FIX_0_765366865;                                                                          \
        t0 = ((i0) + (i4)) * (1 << CONST_BITS);                                                                  \
        t1 = ((i0) - (i4)) * (1 << CONST_BITS);                                                                  \
        t10 = t0 + t3;                                                                                           \
        t13 = t0 - t3;                                                                                           \
        t11 = t1 + t2;                                                                                           \
        t12 = t1 - t2;                                                                                           \
        t0 = (i7);                                                                                               \
        t1 = (i5);                                                                                               \
        t2 = (i3);                                                                                               \
        t3 = (i1);                                                                                               \
        z1 = t0 + t3;                                                                                            \
        z2 = t1 + t2;                                                                                            \
        z3 = t0 + t2;                                                                                            \
        z4 = t1 + t3;                                                                                            \
        z5 = (z3 + z4) * FIX_1_175875602;                                                                        \
        t0 *= FIX_0_298631336;                                                                                   \
        t1 *= FIX_2_053119869;                                                                                   \
        t2 *= FIX_3_072711026;                                                                                   \
        t3 *= FIX_1_501321110;                                                                                   \
        z1 *= -FIX_0_899976223;                                                                                  \
        z2 *= -FIX_2_562915447;                                                                                  \
        z3 = z3 * -FIX_1_961570560 + z5;                                                                         \
        z4 = z4 * -FIX_0_390180644 + z5;                                                                         \
        t0 += z1 + z3;                                                                                           \
        t1 += z2 + z4;                                                                                           \
        t2 += z2 + z3;                                                                                           \
        t3 += z1 + z4;                                                                                           \
        o[0] = t10 + t3;                                                                                         \
        o[7] = t10 - t3;                                                                                         \
        o[1] = t11 + t2;                                                                                         \
        o[6] = t11 - t2;                                                                                         \
        o[2] = t12 + t1;                                                                                         \
        o[5] = t12 - t1;                                                                                         \
        o[3] = t13 + t0;                                                                                         \
        o[4] = t13 - t0;                                                                                         \
    } while (0)

// Row r of an 8x8 block in GX I8 tiles (8x4 texels, 32 bytes): rows 4-7 lie one tile row below.
static inline u8* tile_row(u8* base, u32 pitch, u32 r)
{
    return base + (r & 3) * 8 + (r >> 2) * pitch * 4;
}

// Dequantizes, transforms and stores the block at (x, y) of a w x h plane in I8 tiles.
static void put_block(const s16 in[64], const u16 q[64], s32 last, u8* plane, u32 w, u32 h, u32 x, u32 y)
{
    u8* base = plane + y * w + x * 4;
    s32 ws[64], o[8];
    u32 r, c, rows;

    if (x + 8 > w || y >= h) {
        return;
    }
    rows = h - y < 8 ? h - y : 8;
    if (last == 0) {
        // Only DC: islow's result for it, without the passes.
        u32 v = clamp8(DESCALE(in[0] * q[0] * (1 << PASS1_BITS), PASS1_BITS + 3) + 128) * 0x01010101u;

        for (r = 0; r < rows; r++) {
            u32* d = (u32*) tile_row(base, w, r);

            d[0] = v;
            d[1] = v;
        }
        return;
    }
    // Pass 1: columns, dequantized, into ws with PASS1_BITS more precision.
    for (c = 0; c < 8; c++) {
        const s16* i = in + c;
        const u16* k = q + c;

        if ((i[8] | i[16] | i[24] | i[32] | i[40] | i[48] | i[56]) == 0) {
            s32 dc = i[0] * k[0] * (1 << PASS1_BITS);

            for (r = 0; r < 8; r++) {
                ws[r * 8 + c] = dc;
            }
            continue;
        }
        IDCT_1D(i[0] * k[0], i[8] * k[8], i[16] * k[16], i[24] * k[24], i[32] * k[32], i[40] * k[40],
                i[48] * k[48], i[56] * k[56], o);
        for (r = 0; r < 8; r++) {
            ws[r * 8 + c] = DESCALE(o[r], CONST_BITS - PASS1_BITS);
        }
    }
    // Pass 2: rows, level shift and clamp.
    for (r = 0; r < rows; r++) {
        const s32* w8 = ws + r * 8;
        u8* d = tile_row(base, w, r);

        if ((w8[1] | w8[2] | w8[3] | w8[4] | w8[5] | w8[6] | w8[7]) == 0) {
            u8 v = clamp8(DESCALE(w8[0], PASS1_BITS + 3) + 128);

            memset(d, v, 8);
            continue;
        }
        IDCT_1D(w8[0], w8[1], w8[2], w8[3], w8[4], w8[5], w8[6], w8[7], o);
        for (c = 0; c < 8; c++) {
            d[c] = clamp8(DESCALE(o[c], CONST_BITS + PASS1_BITS + 3) + 128);
        }
    }
}

// Y is w x rows, U and V half that, all in I8 tiles. MCUs are Y0 Y1 Y2 Y3 U V.
static void decode(THPFileInfo* info, u8* ty, u8* tu, u8* tv, u32 w)
{
    Dec* d = dec_of(info);
    Bits b = { d->scan, 0, 0 };
    s16 pred[3] = { 0, 0, 0 };
    s16 blk[64];
    u32 mcux = (d->width + 15u) / 16, mcuy = (d->rows + 15u) / 16, left = d->restart, mx, my, i;
    const Huff* dc[3];
    const Huff* ac[3];

    for (i = 0; i < 3; i++) {
        dc[i] = &d->huff[d->dcsel[i] * 2];
        ac[i] = &d->huff[d->acsel[i] * 2 + 1];
    }
    for (my = 0; my < mcuy; my++) {
        for (mx = 0; mx < mcux; mx++) {
            for (i = 0; i < 4; i++) {
                s32 last = decode_block(&b, dc[0], ac[0], &pred[0], blk);

                put_block(blk, d->qt[d->qsel[0]], last, ty, w, d->rows, mx * 16 + (i & 1) * 8,
                          my * 16 + (i >> 1) * 8);
            }
            for (i = 1; i < 3; i++) {
                s32 last = decode_block(&b, dc[i], ac[i], &pred[i], blk);

                put_block(blk, d->qt[d->qsel[i]], last, i == 1 ? tu : tv, w / 2, d->rows / 2u, mx * 8, my * 8);
            }
            if (d->restart != 0 && --left == 0) {
                // As THPDec.c: the next byte boundary, with no marker to skip.
                left = d->restart;
                skip(&b, b.n & 7);
                pred[0] = pred[1] = pred[2] = 0;
            }
        }
    }
    // The GX texture cache samples large textures for its hash; the new frame must not be missed.
    gp_post_mem_changed(ty, w * d->rows);
    gp_post_mem_changed(tu, w / 2 * (d->rows / 2u));
    gp_post_mem_changed(tv, w / 2 * (d->rows / 2u));
}

void THPDec_80331340(THPFileInfo* info, void* tileY, void* tileU, void* tileV)
{
    decode(info, tileY, tileU, tileV, 640);
}

void THPDec_803313D0(THPFileInfo* info, void* tileY, void* tileU, void* tileV, u32 width)
{
    decode(info, tileY, tileU, tileV, width);
}
