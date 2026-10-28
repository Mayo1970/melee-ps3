// GX vertices to RSX vertex attributes. Each component keeps its GX type where the RSX has one (f32, s16; u8
// and s8 as s16), so the vertex program dequantizes; indexed attributes are fetched from their arrays.
#ifndef PS3_GX_VTX_H
#define PS3_GX_VTX_H

#include "gp.h"

// RSX vertex data types. PSL1GHT names only F32 and U8; 5 and 7 are the NV40's s16 and u8, unnormalized.
// Type 7 only with 4 components (nouveau's NV30 table): a 2-component one stopped the RSX in Melee run 12.
#define VTX_F32 2
#define VTX_U8_NORM 4
#define VTX_S16 5
#define VTX_U8 7

#define VTX_MAX_STEPS 40
#define VTX_ARRAYS 12

typedef struct VtxStep {
    u8 op;
    u8 src;      // offset in the GX vertex of the data, or of the index
    u8 dst;      // offset in the RSX vertex
    u8 n;        // components (bytes for a copy)
    u8 array;    // array of an indexed attribute; 0xFF when direct
    u8 idx16;    // the index is 16-bit
    u8 elem_off; // byte offset in the array element (NBT3 parts)
} VtxStep;

typedef struct VtxLayout {
    u32 vcd_lo, vcd_hi, vat_a, vat_b, vat_c;
    u16 in_size, out_size;
    u16 attrs; // RSX attribute slots present (xfgen.h VA_*)
    u8 pos3;
    struct {
        u8 offset, type, count;
    } attr[16];
    u8 nsteps;
    VtxStep step[VTX_MAX_STEPS];
    // Dequantization: position, normal, texcoord 0-7.
    float dq[10];
    // Bytes read from an element of each attribute array (0 position ... 11 texcoord 7); 0 when not indexed.
    u8 extent[VTX_ARRAYS];
    // Steps that read the default matrix indices (XF 0x18, 0x19).
    bool midx_def;
} VtxLayout;

// The layout for VAT vat under the current VCD.
void vtx_layout(const GpState* s, int vat, VtxLayout* l);
// count GX vertices into RSX vertices of l->out_size bytes. False when an array address maps to nothing.
bool vtx_convert(const VtxLayout* l, const GpState* s, const u8* in, u32 count, u8* out);
// Widens lo[i] and hi[i] to the indices that count GX vertices at in read from array i.
void vtx_index_range(const VtxLayout* l, const u8* in, u32 count, u32 lo[VTX_ARRAYS], u32 hi[VTX_ARRAYS]);

#endif
