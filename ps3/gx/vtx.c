// GX vertices to RSX vertex attributes. See vtx.h. Attribute order and formats as gp_vertex_size reads them.
#include "vtx.h"

#include <string.h>

#include <dolphin/os.h>

#include "xfgen.h"

enum {
    OP_COPY,     // n bytes
    OP_S8_S16,   // n components
    OP_U8_S16,   // n components
    OP_U16_F32,  // n components
    OP_RGB565,
    OP_RGB8,
    OP_RGBX8,
    OP_RGBA4,
    OP_RGBA6,
    OP_RGBA8,
    OP_MIDX_DEF, // the default matrix index src (0 position, 1 + n texcoord n)
};

enum { FMT_U8, FMT_S8, FMT_U16, FMT_S16, FMT_F32 };

static const u8 comp_size[8] = { 1, 1, 2, 2, 4, 0, 0, 0 };
static const u8 color_size[8] = { 2, 3, 4, 2, 3, 4, 0, 0 };

typedef struct Builder {
    VtxLayout* l;
    u32 src, dst;
} Builder;

static void add_step(Builder* b, u8 op, u32 src, u8 n, u8 array, u8 idx16, u8 elem_off)
{
    VtxStep* st;

    if (b->l->nsteps >= VTX_MAX_STEPS) {
        return;
    }
    st = &b->l->step[b->l->nsteps++];
    st->op = op;
    st->src = (u8) src;
    st->dst = (u8) b->dst;
    st->n = n;
    st->array = array;
    st->idx16 = idx16;
    st->elem_off = elem_off;
}

// A new RSX attribute at the next 4-byte aligned offset.
static void open_attr(Builder* b, int slot, u8 type, u8 count, u32 bytes)
{
    b->dst = (b->dst + 3) & ~3u;
    b->l->attrs |= (u16) (1u << slot);
    b->l->attr[slot].offset = (u8) b->dst;
    b->l->attr[slot].type = type;
    b->l->attr[slot].count = count;
    (void) bytes;
}

// A component vector of GX format fmt: its RSX type, and the step that writes it at b->dst.
static void add_vector(Builder* b, int slot, int fmt, u8 count, u32 src, u8 array, u8 idx16, u8 elem_off)
{
    switch (fmt) {
    case FMT_F32:
        open_attr(b, slot, VTX_F32, count, 4u * count);
        add_step(b, OP_COPY, src, (u8) (4 * count), array, idx16, elem_off);
        b->dst += 4u * count;
        break;
    case FMT_S16:
        open_attr(b, slot, VTX_S16, count, 2u * count);
        add_step(b, OP_COPY, src, (u8) (2 * count), array, idx16, elem_off);
        b->dst += 2u * count;
        break;
    case FMT_U8:
        open_attr(b, slot, VTX_S16, count, 2u * count);
        add_step(b, OP_U8_S16, src, count, array, idx16, elem_off);
        b->dst += 2u * count;
        break;
    case FMT_S8:
        open_attr(b, slot, VTX_S16, count, 2u * count);
        add_step(b, OP_S8_S16, src, count, array, idx16, elem_off);
        b->dst += 2u * count;
        break;
    default:
        open_attr(b, slot, VTX_F32, count, 4u * count);
        add_step(b, OP_U16_F32, src, count, array, idx16, elem_off);
        b->dst += 4u * count;
        break;
    }
}

// Texcoord n's count bit, format and fraction bits, from VAT A to C.
static void tex_format(const VtxLayout* l, int n, u32* count, u32* fmt, u32* frac)
{
    static const u8 word[8] = { 0, 1, 1, 1, 1, 2, 2, 2 };
    static const u8 shift[8] = { 21, 0, 9, 18, 27, 5, 14, 23 };
    u32 v = word[n] == 0 ? l->vat_a : word[n] == 1 ? l->vat_b : l->vat_c;

    *count = (v >> shift[n]) & 1;
    *fmt = (v >> (shift[n] + 1)) & 7;
    if (n == 4) {
        *frac = l->vat_c & 0x1F;
    } else {
        *frac = (v >> (shift[n] + 4)) & 0x1F;
    }
}

static float frac_scale(u32 fmt, u32 frac)
{
    return fmt == FMT_F32 ? 1.0f : 1.0f / (float) (1u << frac);
}

void vtx_layout(const GpState* s, int vat, VtxLayout* l)
{
    Builder b;
    u32 lo = s->vcd_lo, hi = s->vcd_hi, a, t, midx_src[9], i;
    bool any_a = false, any_b = false;

    memset(l, 0, sizeof(*l));
    l->vcd_lo = lo;
    l->vcd_hi = hi;
    l->vat_a = a = s->vat_a[vat];
    l->vat_b = s->vat_b[vat];
    l->vat_c = s->vat_c[vat];
    b.l = l;
    b.src = 0;
    b.dst = 0;
    // Matrix indices come first in the GX vertex: one byte each, direct.
    for (i = 0; i < 9; i++) {
        midx_src[i] = 0xFFFF;
        if ((lo >> i) & 1) {
            midx_src[i] = b.src++;
            if (i < 4) {
                any_a = true;
            } else if (i < 8) {
                any_b = true;
            }
        }
    }
    if (any_a || any_b) {
        // MIDX_A is (position, texcoord 0-2); a missing one takes the MATINDEX default.
        open_attr(&b, VA_MIDX_A, VTX_U8, 4, 4);
        for (i = 0; i < 4; i++) {
            if (midx_src[i] != 0xFFFF) {
                add_step(&b, OP_COPY, midx_src[i], 1, 0xFF, 0, 0);
            } else {
                add_step(&b, OP_MIDX_DEF, i, 1, 0xFF, 0, 0);
            }
            b.dst++;
        }
    }
    if (any_b) {
        open_attr(&b, VA_MIDX_B, VTX_U8, 4, 4);
        for (i = 4; i < 8; i++) {
            if (midx_src[i] != 0xFFFF) {
                add_step(&b, OP_COPY, midx_src[i], 1, 0xFF, 0, 0);
            } else {
                add_step(&b, OP_MIDX_DEF, i, 1, 0xFF, 0, 0);
            }
            b.dst++;
        }
    }
    // Position.
    t = (lo >> 9) & 3;
    if (t != 0) {
        u32 fmt = (a >> 1) & 7, cnt = (a & 1) ? 3 : 2;

        l->pos3 = cnt == 3;
        l->dq[0] = frac_scale(fmt, (a >> 4) & 0x1F);
        if (t == 1) {
            add_vector(&b, VA_POS, fmt, (u8) cnt, b.src, 0xFF, 0, 0);
            b.src += comp_size[fmt] * cnt;
        } else {
            add_vector(&b, VA_POS, fmt, (u8) cnt, b.src, 0, t == 3, 0);
            b.src += t - 1;
        }
    }
    // Normal, binormal, tangent.
    t = (lo >> 11) & 3;
    if (t != 0) {
        u32 fmt = (a >> 10) & 7, csz = comp_size[fmt];
        bool nbt = (a >> 9) & 1, nbt3 = nbt && (a >> 31);
        u32 parts = nbt ? 3 : 1, p;
        static const u8 slots[3] = { VA_NRM, VA_BINRM, VA_TANGENT };

        l->dq[1] = fmt == FMT_F32 ? 1.0f : csz == 1 ? 1.0f / 64.0f : 1.0f / 16384.0f;
        for (p = 0; p < parts; p++) {
            if (t == 1) {
                add_vector(&b, slots[p], fmt, 3, b.src + p * 3 * csz, 0xFF, 0, 0);
            } else {
                u32 isz = t - 1;

                add_vector(&b, slots[p], fmt, 3, b.src + (nbt3 ? p * isz : 0), 1, t == 3, (u8) (p * 3 * csz));
            }
        }
        b.src += t == 1 ? 3 * parts * csz : (t - 1) * (nbt3 ? 3 : 1);
    }
    // Colours: four normalized bytes on the RSX.
    for (i = 0; i < 2; i++) {
        static const u8 ops[8] = { OP_RGB565, OP_RGB8, OP_RGBX8, OP_RGBA4, OP_RGBA6, OP_RGBA8, OP_RGBA8, OP_RGBA8 };
        u32 fmt = (a >> (14 + 4 * i)) & 7;

        t = (lo >> (13 + 2 * i)) & 3;
        if (t == 0) {
            continue;
        }
        open_attr(&b, VA_COL0 + i, VTX_U8_NORM, 4, 4);
        if (t == 1) {
            add_step(&b, ops[fmt], b.src, 4, 0xFF, 0, 0);
            b.src += color_size[fmt];
        } else {
            add_step(&b, ops[fmt], b.src, 4, (u8) (2 + i), t == 3, 0);
            b.src += t - 1;
        }
        b.dst += 4;
    }
    // Texcoords.
    for (i = 0; i < 8; i++) {
        u32 count, fmt, frac, n;

        t = (hi >> (2 * i)) & 3;
        if (t == 0) {
            continue;
        }
        tex_format(l, i, &count, &fmt, &frac);
        n = count ? 2 : 1;
        l->dq[2 + i] = frac_scale(fmt, frac);
        if (t == 1) {
            add_vector(&b, VA_TEX0 + i, fmt, (u8) n, b.src, 0xFF, 0, 0);
            b.src += comp_size[fmt] * n;
        } else {
            add_vector(&b, VA_TEX0 + i, fmt, (u8) n, b.src, (u8) (4 + i), t == 3, 0);
            b.src += t - 1;
        }
    }
    l->in_size = (u16) b.src;
    // A multiple of 4 bytes, so every float stays aligned.
    l->out_size = (u16) ((b.dst + 3) & ~3u);
}

static inline u16 be16(const u8* p)
{
    return (u16) (p[0] << 8 | p[1]);
}

static inline void put_rgba(u8* d, u32 r, u32 g, u32 b, u32 a)
{
    d[0] = (u8) r;
    d[1] = (u8) g;
    d[2] = (u8) b;
    d[3] = (u8) a;
}

static inline u32 x5(u32 v)
{
    return v << 3 | v >> 2;
}

static inline u32 x6(u32 v)
{
    return v << 2 | v >> 4;
}

bool vtx_convert(const VtxLayout* l, const GpState* s, const u8* in, u32 count, u8* out)
{
    const u8* arrays[16];
    u32 used = 0, i, v;
    u8 def[9];

    for (i = 0; i < l->nsteps; i++) {
        if (l->step[i].array != 0xFF) {
            used |= 1u << l->step[i].array;
        }
    }
    for (i = 0; i < 16; i++) {
        arrays[i] = NULL;
        if ((used >> i) & 1) {
            arrays[i] = OSPhysicalToCached(s->array_base[i]);
            if (arrays[i] == NULL) {
                return false;
            }
        }
    }
    def[0] = (u8) (s->xf_regs[0x18] & 0x3F);
    for (i = 0; i < 4; i++) {
        def[1 + i] = (u8) ((s->xf_regs[0x18] >> (6 + 6 * i)) & 0x3F);
        def[5 + i] = (u8) ((s->xf_regs[0x19] >> (6 * i)) & 0x3F);
    }
    for (v = 0; v < count; v++, in += l->in_size, out += l->out_size) {
        for (i = 0; i < l->nsteps; i++) {
            const VtxStep* st = &l->step[i];
            const u8* p = in + st->src;
            u8* d = out + st->dst;
            u32 k, c;

            if (st->array != 0xFF) {
                u32 idx = st->idx16 ? be16(p) : p[0];

                p = arrays[st->array] + idx * s->array_stride[st->array] + st->elem_off;
            }
            switch (st->op) {
            case OP_COPY:
                memcpy(d, p, st->n);
                break;
            case OP_S8_S16:
                for (k = 0; k < st->n; k++) {
                    d[2 * k] = (p[k] & 0x80) ? 0xFF : 0;
                    d[2 * k + 1] = p[k];
                }
                break;
            case OP_U8_S16:
                for (k = 0; k < st->n; k++) {
                    d[2 * k] = 0;
                    d[2 * k + 1] = p[k];
                }
                break;
            case OP_U16_F32:
                for (k = 0; k < st->n; k++) {
                    float f = (float) be16(p + 2 * k);
                    memcpy(d + 4 * k, &f, 4);
                }
                break;
            case OP_RGB565:
                c = be16(p);
                put_rgba(d, x5(c >> 11), x6((c >> 5) & 0x3F), x5(c & 0x1F), 0xFF);
                break;
            case OP_RGB8:
            case OP_RGBX8:
                put_rgba(d, p[0], p[1], p[2], 0xFF);
                break;
            case OP_RGBA4:
                c = be16(p);
                put_rgba(d, (c >> 12) * 17, ((c >> 8) & 0xF) * 17, ((c >> 4) & 0xF) * 17, (c & 0xF) * 17);
                break;
            case OP_RGBA6:
                c = (u32) p[0] << 16 | (u32) p[1] << 8 | p[2];
                put_rgba(d, x6(c >> 18), x6((c >> 12) & 0x3F), x6((c >> 6) & 0x3F), x6(c & 0x3F));
                break;
            case OP_RGBA8:
                memcpy(d, p, 4);
                break;
            default:
                d[0] = def[st->src];
                break;
            }
        }
    }
    return true;
}
