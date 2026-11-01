// The GameCube XF (transform, lighting, texgen) as an RSX vertex program, one per XF state. The math follows
// Dolphin's VertexShaderGen and LightingShaderGen; matrices, lights and colours are vertex program constants.
#ifndef PS3_GX_XFGEN_H
#define PS3_GX_XFGEN_H

#include "gp.h"
#include "vpgen.h"

// RSX vertex attribute slots.
enum {
    VA_POS = 0,
    VA_MIDX_A = 1, // matrix indices: position/normal, texcoord 0-2 (u8)
    VA_NRM = 2,
    VA_COL0 = 3,
    VA_COL1 = 4,
    VA_BINRM = 5,
    VA_TANGENT = 6,
    VA_MIDX_B = 7, // texcoord 3-6 matrix indices (u8)
    VA_TEX0 = 8,
};

// Vertex program constants.
enum {
    C_MTX = 0,     // 64 rows: position and texture matrices (XF 0x000-0x0FF)
    C_NRM = 64,    // 32 rows: normal matrices (XF 0x400-0x45F), w = 0
    C_POST = 96,   // 64 rows: post-transform matrices (XF 0x500-0x5FF)
    C_LIGHT = 160, // 8 lights: colour, cos attenuation, distance attenuation, position, direction
    C_PROJ = 200,  // 4 rows: projection
    C_CHAN = 204,  // ambient 0, ambient 1, material 0, material 1
    C_DQ = 208,    // dequantization: (position, normal, tex0, tex1), (tex2-5), (tex6, tex7, 0, 0)
    C_MIDX = 211,  // matrix indices when the vertex has none: (pn, t0, t1, t2), (t3, t4, t5, t6), (t7, 1e-30, 0, 0)
    C_MISC = 214,  // (0, 1, 0.5, 2)
    C_TEV = 215,   // XF_TEV_CONSTS values for the fragment program
    C_COUNT = 225,
};
#define LIGHT_STRIDE 5
// K0-K3, the start values of PREV and REG0-REG2, fog parameters, fog colour.
#define XF_TEV_CONSTS 10

typedef struct XfKey {
    u16 attrs; // bit n: RSX attribute n present
    u8 pos3;   // the position has z
    // Depth out as z + w (before run 54), not -z: an A/B mode keeps it, with the viewport to match (backend.c).
    u8 old_depth;
    u8 numchans, numtexgens, dualtex;
    // First texcoord output of the TEV constants. The RSX has texcoord outputs 0-7 only: tev_first plus the
    // number of TEV constants must stay within 8.
    u8 tev_first;
    u16 tev_consts; // bit n: TEV constant n goes out, in order from tev_first
    u32 chan[4];    // XF colour 0, colour 1, alpha 0, alpha 1 control
    u32 texinfo[8], postinfo[8];
} XfKey;

// The key for the current XF registers. attrs and pos3 come from the vertex layout.
void xf_make_key(const GpState* s, u16 attrs, bool pos3, u16 tev_consts, XfKey* key);
// Returns the instruction count, or -1.
int xf_gen(const XfKey* key, VpProg* p);
// The XF registers the constants read: 0x0A-0x0D, 0x18-0x19 and the viewport 0x1A-0x1F in xf_reg_dirty[0],
// 0x20-0x26 in xf_reg_dirty[1].
#define XF_VIEWPORT_REGS 0xFC000000u
#define XF_CONST_REGS_0 (0x03003C00u | XF_VIEWPORT_REGS)
#define XF_CONST_REGS_1 0x0000007Fu

// Perspective projections into the whole EFB width show 16:9 in its 640 pixels, which the present widens.
extern bool xf_widescreen;

// The XF part of the constants (C_MTX to C_MISC) as GpState's dirty bits mem_dirty and reg_dirty mark, or all of it
// when they are NULL. Rows that differ are written as float bits and marked in changed; C_DQ and C_TEV stay.
void xf_constants(const GpState* s, const u32* mem_dirty, const u32* reg_dirty, float c[C_COUNT][4], u32* changed);

// CPU reference of the program, for the tests: attributes as the RSX hands them (before dequantization).
typedef struct XfIn {
    float a[16][4];
} XfIn;
typedef struct XfOut {
    float pos[4], col[2][4], tc[8][4];
} XfOut;
void xf_ref(const XfKey* key, const float c[C_COUNT][4], const XfIn* in, XfOut* out);

// rsxcheck: adds programs for a set of keys, with relative addressing written as plain constants.
void xf_check_cases(VpProg* p, void (*add)(const char* name));

#endif
