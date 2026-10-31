// The steps that turn one GX vertex into an RSX vertex (vtx.c), shared with the vertex program on the SPU
// (vxspu.c): stdint types only.
#ifndef PS3_GX_VTX_STEP_H
#define PS3_GX_VTX_STEP_H

#include <stdint.h>
#include <string.h>

enum {
    VTX_OP_COPY,     // n bytes
    VTX_OP_S8_S16,   // n components
    VTX_OP_U8_S16,   // n components
    VTX_OP_U16_F32,  // n components
    VTX_OP_RGB565,
    VTX_OP_RGB8,
    VTX_OP_RGBX8,
    VTX_OP_RGBA4,
    VTX_OP_RGBA6,
    VTX_OP_RGBA8,
    VTX_OP_MIDX_DEF, // the default matrix index src (0 position, 1 + n texcoord n)
};

typedef struct VtxStep {
    uint8_t op;
    uint8_t src;      // offset in the GX vertex of the data, or of the index
    uint8_t dst;      // offset in the RSX vertex
    uint8_t n;        // components (bytes for a copy)
    uint8_t array;    // array of an indexed attribute; 0xFF when direct
    uint8_t idx16;    // the index is 16-bit
    uint8_t elem_off; // byte offset in the array element (NBT3 parts)
} VtxStep;

static inline uint16_t vtx_be16(const uint8_t* p)
{
    return (uint16_t) (p[0] << 8 | p[1]);
}

// The source bytes a step reads.
static inline uint32_t vtx_step_bytes(const VtxStep* st)
{
    static const uint8_t color_bytes[6] = { 2, 3, 4, 2, 3, 4 };

    if (st->op == VTX_OP_MIDX_DEF) {
        return 0;
    }
    if (st->op >= VTX_OP_RGB565) {
        return color_bytes[st->op - VTX_OP_RGB565];
    }
    return st->op == VTX_OP_U16_F32 ? 2u * st->n : st->n;
}

static inline void vtx_put_rgba(uint8_t* d, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    d[0] = (uint8_t) r;
    d[1] = (uint8_t) g;
    d[2] = (uint8_t) b;
    d[3] = (uint8_t) a;
}

static inline uint32_t vtx_x5(uint32_t v)
{
    return v << 3 | v >> 2;
}

static inline uint32_t vtx_x6(uint32_t v)
{
    return v << 2 | v >> 4;
}

// Step st with its source data at p into the RSX vertex at out; def holds the default matrix indices.
static inline void vtx_step_run(const VtxStep* st, const uint8_t* p, uint8_t* out, const uint8_t def[9])
{
    uint8_t* d = out + st->dst;
    uint32_t k, c;

    switch (st->op) {
    case VTX_OP_COPY:
        memcpy(d, p, st->n);
        break;
    case VTX_OP_S8_S16:
        for (k = 0; k < st->n; k++) {
            d[2 * k] = (p[k] & 0x80) ? 0xFF : 0;
            d[2 * k + 1] = p[k];
        }
        break;
    case VTX_OP_U8_S16:
        for (k = 0; k < st->n; k++) {
            d[2 * k] = 0;
            d[2 * k + 1] = p[k];
        }
        break;
    case VTX_OP_U16_F32:
        for (k = 0; k < st->n; k++) {
            float f = (float) vtx_be16(p + 2 * k);
            memcpy(d + 4 * k, &f, 4);
        }
        break;
    case VTX_OP_RGB565:
        c = vtx_be16(p);
        vtx_put_rgba(d, vtx_x5(c >> 11), vtx_x6((c >> 5) & 0x3F), vtx_x5(c & 0x1F), 0xFF);
        break;
    case VTX_OP_RGB8:
    case VTX_OP_RGBX8:
        vtx_put_rgba(d, p[0], p[1], p[2], 0xFF);
        break;
    case VTX_OP_RGBA4:
        c = vtx_be16(p);
        vtx_put_rgba(d, (c >> 12) * 17, ((c >> 8) & 0xF) * 17, ((c >> 4) & 0xF) * 17, (c & 0xF) * 17);
        break;
    case VTX_OP_RGBA6:
        c = (uint32_t) p[0] << 16 | (uint32_t) p[1] << 8 | p[2];
        vtx_put_rgba(d, vtx_x6(c >> 18), vtx_x6((c >> 12) & 0x3F), vtx_x6((c >> 6) & 0x3F), vtx_x6(c & 0x3F));
        break;
    case VTX_OP_RGBA8:
        memcpy(d, p, 4);
        break;
    default:
        d[0] = def[st->src];
        break;
    }
}

#endif
