// The GameCube XF as an RSX vertex program. See xfgen.h.
#include "xfgen.h"

#include <math.h>
#include <string.h>

// Temps.
enum {
    T_POS,  // model position, w = 1
    T_EYE,  // eye position, w = 1
    T_NRM,  // eye normal, normalized
    T_CLIP,
    T_LDIR, // light direction; w: distance squared
    T_S,    // light term scratch: x the result
    T_V,    // attenuation polynomial (1, a, a^2)
    T_LACC,
    T_MAT,
    T_K,    // normalized distance attenuation
    T_RAWN, // model normal
    T_SRC,  // texgen source
    T_TC,   // texgen result
    T_COL0,
    T_COL1,
};

enum { ATTN_NONE, ATTN_SPEC, ATTN_SPOT };
enum { DIFF_NONE, DIFF_SIGN, DIFF_CLAMP };

typedef struct Chan {
    bool matvtx, enable, ambvtx;
    u8 mask, diff, attn;
} Chan;

static Chan decode_chan(u32 r)
{
    Chan c;
    bool en = (r >> 9) & 1, spot = (r >> 10) & 1;

    c.matvtx = r & 1;
    c.enable = (r >> 1) & 1;
    c.ambvtx = (r >> 6) & 1;
    c.mask = (u8) (((r >> 2) & 0xF) | (((r >> 11) & 0xF) << 4));
    c.diff = (u8) ((r >> 7) & 3);
    c.attn = !en ? ATTN_NONE : spot ? ATTN_SPOT : ATTN_SPEC;
    return c;
}

static bool has(const XfKey* key, int attr)
{
    return (key->attrs >> attr) & 1;
}

// Dequantization scale n (0 position, 1 normal, 2 + t texcoord t) replicated.
static VpSrc dq(int n)
{
    static const char* rep[4] = { "xxxx", "yyyy", "zzzz", "wwww" };
    return vp_c(C_DQ + n / 4, rep[n % 4]);
}

static VpSrc one(void)
{
    return vp_c(C_MISC, "yyyy");
}

static VpSrc zero(void)
{
    return vp_c(C_MISC, "xxxx");
}

// GX's material and ambient colour from the vertex: its own colour, else colour 0, else white.
static VpSrc vtx_color(const XfKey* key, int j)
{
    if (has(key, VA_COL0 + j)) {
        return vp_in(VA_COL0 + j, NULL);
    }
    if (has(key, VA_COL0)) {
        return vp_in(VA_COL0, NULL);
    }
    return one();
}

// T_S.w = 1 / T_S.w, a divisor of 0 raised to C_MIDX + 2's y: GX's attenuation gives 0 for 0 / 0 (Dolphin's
// SafeDivide), where RCP(0) times 0 is NaN, a black vertex colour. T_V is scratch.
static void safe_rcp(VpProg* p)
{
    VpSrc z = vp_t(0, NULL);

    // -|d| >= 0 only for d == 0.
    vp_op(p, VOP_SGE, T_V, 1, false, vp_neg(vp_abs(vp_t(T_S, "wwww"))), zero(), z);
    vp_op(p, VOP_MAD, T_S, 8, false, vp_t(T_V, "xxxx"), vp_c(C_MIDX + 2, "yyyy"), vp_t(T_S, NULL));
    vp_op(p, VOP_RCP, T_S, 8, false, vp_t(T_S, "w"), z, z);
}

// Normalizes reg.xyz with T_S.w as scratch.
static void normalize(VpProg* p, int reg)
{
    VpSrc z = vp_t(0, NULL);

    vp_op(p, VOP_DP3, T_S, 8, false, vp_t(reg, NULL), vp_t(reg, NULL), z);
    vp_op(p, VOP_RSQ, T_S, 8, false, vp_t(T_S, "w"), z, z);
    vp_op(p, VOP_MUL, reg, 7, false, vp_t(reg, NULL), vp_t(T_S, "wwww"), z);
}

// The factor of light i for a channel, into T_S.x. As Dolphin's GenerateLightShader.
static void light_term(VpProg* p, int i, const Chan* c)
{
    VpSrc z = vp_t(0, NULL);
    int l = C_LIGHT + LIGHT_STRIDE * i;

    vp_op(p, VOP_ADD, T_LDIR, 7, false, vp_c(l + 3, NULL), vp_neg(vp_t(T_EYE, NULL)), z);
    if (c->attn == ATTN_SPOT) {
        vp_op(p, VOP_DP3, T_LDIR, 8, false, vp_t(T_LDIR, NULL), vp_t(T_LDIR, NULL), z);
        vp_op(p, VOP_RSQ, T_S, 8, false, vp_t(T_LDIR, "w"), z, z);
        vp_op(p, VOP_MUL, T_LDIR, 7, false, vp_t(T_LDIR, NULL), vp_t(T_S, "wwww"), z);
        vp_op(p, VOP_RCP, T_S, 4, false, vp_t(T_S, "w"), z, z);
        vp_op(p, VOP_DP3, T_S, 2, false, vp_t(T_LDIR, NULL), vp_c(l + 4, NULL), z);
        vp_op(p, VOP_MAX, T_S, 2, false, vp_t(T_S, NULL), zero(), z);
        // (1, a, a^2) against the cos attenuation, (1, dist, dist^2) against the distance attenuation.
        vp_op(p, VOP_MOV, T_V, 1, false, one(), z, z);
        vp_op(p, VOP_MOV, T_V, 2, false, vp_t(T_S, "yyyy"), z, z);
        vp_op(p, VOP_MUL, T_V, 4, false, vp_t(T_S, "yyyy"), vp_t(T_S, "yyyy"), z);
        vp_op(p, VOP_DP3, T_S, 1, false, vp_c(l + 1, NULL), vp_t(T_V, NULL), z);
        vp_op(p, VOP_MAX, T_S, 1, false, vp_t(T_S, NULL), zero(), z);
        vp_op(p, VOP_MOV, T_V, 2, false, vp_t(T_S, "zzzz"), z, z);
        vp_op(p, VOP_MOV, T_V, 4, false, vp_t(T_LDIR, "wwww"), z, z);
        vp_op(p, VOP_DP3, T_S, 8, false, vp_c(l + 2, NULL), vp_t(T_V, NULL), z);
        safe_rcp(p);
        vp_op(p, VOP_MUL, T_S, 1, false, vp_t(T_S, NULL), vp_t(T_S, "wwww"), z);
    } else {
        normalize(p, T_LDIR);
        if (c->attn == ATTN_SPEC) {
            VpSrc k = vp_c(l + 2, NULL);

            vp_op(p, VOP_DP3, T_S, 1, false, vp_t(T_NRM, NULL), vp_t(T_LDIR, NULL), z);
            vp_op(p, VOP_DP3, T_S, 2, false, vp_t(T_NRM, NULL), vp_c(l + 4, NULL), z);
            vp_op(p, VOP_MAX, T_S, 2, false, vp_t(T_S, NULL), zero(), z);
            vp_op(p, VOP_SGE, T_S, 4, false, vp_t(T_S, "xxxx"), zero(), z);
            vp_op(p, VOP_MUL, T_S, 2, false, vp_t(T_S, NULL), vp_t(T_S, "zzzz"), z);
            vp_op(p, VOP_MOV, T_V, 1, false, one(), z, z);
            vp_op(p, VOP_MOV, T_V, 2, false, vp_t(T_S, "yyyy"), z, z);
            vp_op(p, VOP_MUL, T_V, 4, false, vp_t(T_S, "yyyy"), vp_t(T_S, "yyyy"), z);
            if (c->diff != DIFF_NONE) {
                vp_op(p, VOP_MOV, T_K, 7, false, k, z, z);
                normalize(p, T_K);
                k = vp_t(T_K, NULL);
            }
            vp_op(p, VOP_DP3, T_S, 1, false, vp_c(l + 1, NULL), vp_t(T_V, NULL), z);
            vp_op(p, VOP_MAX, T_S, 1, false, vp_t(T_S, NULL), zero(), z);
            vp_op(p, VOP_DP3, T_S, 8, false, k, vp_t(T_V, NULL), z);
            safe_rcp(p);
            vp_op(p, VOP_MUL, T_S, 1, false, vp_t(T_S, NULL), vp_t(T_S, "wwww"), z);
        } else {
            vp_op(p, VOP_MOV, T_S, 1, false, one(), z, z);
        }
    }
    if (c->diff != DIFF_NONE) {
        vp_op(p, VOP_DP3, T_S, 2, false, vp_t(T_LDIR, NULL), vp_t(T_NRM, NULL), z);
        if (c->diff == DIFF_CLAMP) {
            vp_op(p, VOP_MAX, T_S, 2, false, vp_t(T_S, NULL), zero(), z);
        }
        vp_op(p, VOP_MUL, T_S, 1, false, vp_t(T_S, NULL), vp_t(T_S, "yyyy"), z);
    }
}

static bool uses_lights(const XfKey* key)
{
    int j;

    for (j = 0; j < key->numchans && j < 2; j++) {
        Chan c = decode_chan(key->chan[j]), a = decode_chan(key->chan[2 + j]);

        if ((c.enable && c.mask) || (a.enable && a.mask)) {
            return true;
        }
    }
    return false;
}

static void gen_channel(VpProg* p, const XfKey* key, int j)
{
    VpSrc z = vp_t(0, NULL), vc = vtx_color(key, j);
    Chan cc = decode_chan(key->chan[j]), ac = decode_chan(key->chan[2 + j]);
    int i;

    vp_op(p, VOP_MOV, T_MAT, 7, false, cc.matvtx ? vc : vp_c(C_CHAN + 2 + j, NULL), z, z);
    vp_op(p, VOP_MOV, T_MAT, 8, false, ac.matvtx ? vc : vp_c(C_CHAN + 2 + j, NULL), z, z);
    if (!cc.enable && !ac.enable) {
        vp_op(p, VOP_MOV, T_COL0 + j, 0xF, false, vp_t(T_MAT, NULL), z, z);
        return;
    }
    vp_op(p, VOP_MOV, T_LACC, 7, false, cc.enable ? (cc.ambvtx ? vc : vp_c(C_CHAN + j, NULL)) : one(), z, z);
    vp_op(p, VOP_MOV, T_LACC, 8, false, ac.enable ? (ac.ambvtx ? vc : vp_c(C_CHAN + j, NULL)) : one(), z, z);
    for (i = 0; i < 8; i++) {
        bool inc = cc.enable && ((cc.mask >> i) & 1), ina = ac.enable && ((ac.mask >> i) & 1);
        VpSrc col = vp_c(C_LIGHT + LIGHT_STRIDE * i, NULL);

        if (inc && ina && cc.diff == ac.diff && cc.attn == ac.attn) {
            light_term(p, i, &cc);
            vp_op(p, VOP_MAD, T_LACC, 0xF, false, vp_t(T_S, "xxxx"), col, vp_t(T_LACC, NULL));
            continue;
        }
        if (inc) {
            light_term(p, i, &cc);
            vp_op(p, VOP_MAD, T_LACC, 7, false, vp_t(T_S, "xxxx"), col, vp_t(T_LACC, NULL));
        }
        if (ina) {
            light_term(p, i, &ac);
            vp_op(p, VOP_MAD, T_LACC, 8, false, vp_t(T_S, "xxxx"), col, vp_t(T_LACC, NULL));
        }
    }
    vp_op(p, VOP_MOV, T_LACC, 0xF, true, vp_t(T_LACC, NULL), z, z);
    vp_op(p, VOP_MUL, T_COL0 + j, 0xF, false, vp_t(T_MAT, NULL), vp_t(T_LACC, NULL), z);
}

// Loads A0 with the texture matrix indices of texcoord i and returns the component for it.
static int texmtx_addr(VpProg* p, const XfKey* key, int i)
{
    VpSrc z = vp_t(0, NULL);

    if (i < 3) {
        return 1 + i;
    }
    if (i == 3) {
        vp_op(p, VOP_ARL, VP_A0, 0xF, false, has(key, VA_MIDX_B) ? vp_in(VA_MIDX_B, NULL) : vp_c(C_MIDX + 1, NULL), z,
              z);
    } else if (i == 7) {
        vp_op(p, VOP_ARL, VP_A0, 1, false, vp_c(C_MIDX + 2, NULL), z, z);
        return 0;
    }
    return i - 3;
}

static void gen_texgen(VpProg* p, const XfKey* key, int i)
{
    VpSrc z = vp_t(0, NULL);
    u32 info = key->texinfo[i];
    int proj = (info >> 1) & 1, form = (info >> 2) & 1, type = (info >> 4) & 7, row = (info >> 7) & 0x1F;
    int addr = texmtx_addr(p, key, i), out = VP_OUT(VO_TC0 + i);

    if (type == 0) {
        if (row == 0) {
            vp_op(p, VOP_MOV, T_SRC, 0xF, false, vp_t(T_POS, NULL), z, z);
        } else if (row == 1 || row == 3 || row == 4) {
            int a = row == 1 ? VA_NRM : row == 3 ? VA_BINRM : VA_TANGENT;

            if (has(key, a)) {
                vp_op(p, VOP_MUL, T_SRC, 7, false, vp_in(a, NULL), dq(1), z);
            } else {
                vp_op(p, VOP_MOV, T_SRC, 7, false, zero(), z, z);
            }
            vp_op(p, VOP_MOV, T_SRC, 8, false, one(), z, z);
        } else if (row >= 5 && row <= 12 && has(key, VA_TEX0 + row - 5)) {
            vp_op(p, VOP_MUL, T_SRC, 3, false, vp_in(VA_TEX0 + row - 5, NULL), dq(2 + row - 5), z);
            vp_op(p, VOP_MOV, T_SRC, 0xC, false, one(), z, z);
        } else {
            vp_op(p, VOP_MOV, T_SRC, 3, false, zero(), z, z);
            vp_op(p, VOP_MOV, T_SRC, 0xC, false, one(), z, z);
        }
        if (form == 0) {
            vp_op(p, VOP_MOV, T_SRC, 4, false, one(), z, z);
        }
        vp_op(p, VOP_DP4, T_TC, 1, false, vp_ca(addr, C_MTX, NULL), vp_t(T_SRC, NULL), z);
        vp_op(p, VOP_DP4, T_TC, 2, false, vp_ca(addr, C_MTX + 1, NULL), vp_t(T_SRC, NULL), z);
        if (proj) {
            vp_op(p, VOP_DP4, T_TC, 4, false, vp_ca(addr, C_MTX + 2, NULL), vp_t(T_SRC, NULL), z);
        } else {
            vp_op(p, VOP_MOV, T_TC, 4, false, one(), z, z);
        }
        if (key->dualtex) {
            u32 post = key->postinfo[i];
            int idx = post & 0x3F;

            if ((post >> 8) & 1) {
                normalize(p, T_TC);
            }
            vp_op(p, VOP_MOV, T_TC, 8, false, one(), z, z);
            vp_op(p, VOP_DP4, T_SRC, 1, false, vp_c(C_POST + idx, NULL), vp_t(T_TC, NULL), z);
            vp_op(p, VOP_DP4, T_SRC, 2, false, vp_c(C_POST + idx + 1, NULL), vp_t(T_TC, NULL), z);
            vp_op(p, VOP_DP4, T_SRC, 4, false, vp_c(C_POST + idx + 2, NULL), vp_t(T_TC, NULL), z);
            vp_op(p, VOP_MOV, T_TC, 7, false, vp_t(T_SRC, NULL), z, z);
        }
    } else if (type == 2 || type == 3) {
        vp_op(p, VOP_MOV, T_TC, 3, false, vp_t(type == 2 ? T_COL0 : T_COL1, NULL), z, z);
        vp_op(p, VOP_MOV, T_TC, 4, false, one(), z, z);
    } else {
        // Emboss: not done.
        vp_op(p, VOP_MOV, T_TC, 3, false, zero(), z, z);
        vp_op(p, VOP_MOV, T_TC, 4, false, one(), z, z);
    }
    vp_op(p, VOP_MOV, out, 3, false, vp_t(T_TC, NULL), z, z);
    vp_op(p, VOP_MOV, out, 4, false, zero(), z, z);
    vp_op(p, VOP_MOV, out, 8, false, proj ? vp_t(T_TC, "zzzz") : one(), z, z);
}

int xf_gen(const XfKey* key, VpProg* p)
{
    VpSrc z = vp_t(0, NULL);
    bool lights = uses_lights(key), need_nrm = lights;
    int i, j, slot;

    for (i = 0; i < key->numtexgens; i++) {
        if (((key->texinfo[i] >> 7) & 0x1F) == 1) {
            need_nrm = true;
        }
    }
    vp_init(p);
    vp_op(p, VOP_ARL, VP_A0, 0xF, false, has(key, VA_MIDX_A) ? vp_in(VA_MIDX_A, NULL) : vp_c(C_MIDX, NULL), z, z);
    vp_op(p, VOP_MUL, T_POS, key->pos3 ? 7 : 3, false, vp_in(VA_POS, NULL), dq(0), z);
    if (!key->pos3) {
        vp_op(p, VOP_MOV, T_POS, 4, false, zero(), z, z);
    }
    vp_op(p, VOP_MOV, T_POS, 8, false, one(), z, z);
    for (i = 0; i < 3; i++) {
        vp_op(p, VOP_DP4, T_EYE, 1 << i, false, vp_ca(0, C_MTX + i, NULL), vp_t(T_POS, NULL), z);
    }
    vp_op(p, VOP_MOV, T_EYE, 8, false, one(), z, z);
    for (i = 0; i < 4; i++) {
        vp_op(p, VOP_DP4, T_CLIP, 1 << i, false, vp_c(C_PROJ + i, NULL), vp_t(T_EYE, NULL), z);
    }
    // GX clip z is in [-w, 0]; the RSX clips to [0, w]. -z keeps z's low bits, which z + w lost at stage distance;
    // the viewport's negative z scale then gives GX's far + range * z / w.
    vp_op(p, VOP_MOV, VP_OUT(VO_POS), 0xB, false, vp_t(T_CLIP, NULL), z, z);
    if (key->old_depth) {
        vp_op(p, VOP_ADD, VP_OUT(VO_POS), 4, false, vp_t(T_CLIP, "zzzz"), vp_t(T_CLIP, "wwww"), z);
    } else {
        vp_op(p, VOP_MOV, VP_OUT(VO_POS), 4, false, vp_neg(vp_t(T_CLIP, "zzzz")), z, z);
    }
    if (need_nrm) {
        if (has(key, VA_NRM)) {
            vp_op(p, VOP_MUL, T_RAWN, 7, false, vp_in(VA_NRM, NULL), dq(1), z);
            for (i = 0; i < 3; i++) {
                vp_op(p, VOP_DP3, T_NRM, 1 << i, false, vp_ca(0, C_NRM + i, NULL), vp_t(T_RAWN, NULL), z);
            }
            normalize(p, T_NRM);
        } else {
            vp_op(p, VOP_MOV, T_RAWN, 7, false, zero(), z, z);
            vp_op(p, VOP_MOV, T_NRM, 7, false, zero(), z, z);
        }
    }
    for (j = 0; j < key->numchans && j < 2; j++) {
        gen_channel(p, key, j);
    }
    if (key->numchans == 0) {
        vp_op(p, VOP_MOV, T_COL0, 0xF, false, has(key, VA_COL0) ? vp_in(VA_COL0, NULL) : one(), z, z);
    }
    if (key->numchans < 2) {
        vp_op(p, VOP_MOV, T_COL1, 0xF, false, has(key, VA_COL1) ? vp_in(VA_COL1, NULL) : vp_t(T_COL0, NULL), z, z);
    }
    vp_op(p, VOP_MOV, VP_OUT(VO_COL0), 0xF, false, vp_t(T_COL0, NULL), z, z);
    vp_op(p, VOP_MOV, VP_OUT(VO_COL1), 0xF, false, vp_t(T_COL1, NULL), z, z);
    for (i = 0; i < key->numtexgens && i < 8; i++) {
        gen_texgen(p, key, i);
    }
    for (i = 0, slot = key->tev_first; i < XF_TEV_CONSTS; i++) {
        if ((key->tev_consts >> i) & 1) {
            vp_op(p, VOP_MOV, VP_OUT(VO_TC0 + slot), 0xF, false, vp_c(C_TEV + i, NULL), z, z);
            slot++;
        }
    }
    vp_end(p);
    return p->err ? -1 : p->n;
}

void xf_make_key(const GpState* s, u16 attrs, bool pos3, u16 tev_consts, XfKey* key)
{
    int i;

    memset(key, 0, sizeof(*key));
    key->attrs = attrs;
    key->pos3 = pos3;
    key->numchans = (u8) (s->xf_regs[0x09] & 3);
    key->numtexgens = (u8) (s->xf_regs[0x3F] & 0xF);
    if (key->numtexgens > 8) {
        key->numtexgens = 8;
    }
    key->dualtex = (u8) (s->xf_regs[0x12] & 1);
    key->tev_first = key->numtexgens;
    key->tev_consts = tev_consts;
    for (i = 0; i < 4; i++) {
        key->chan[i] = s->xf_regs[0x0E + i] & 0x7FFF;
    }
    for (i = 0; i < key->numtexgens; i++) {
        key->texinfo[i] = s->xf_regs[0x40 + i] & 0x3FFFE;
        key->postinfo[i] = key->dualtex ? s->xf_regs[0x50 + i] & 0x13F : 0;
    }
}

// The float bits of (float) i / 255.0f, made with that expression at the first call: no int-to-float conversion
// through memory, no divides.
static u32 unorm8[256];
static bool unorm8_ready;

static void unorm8_init(void)
{
    u32 i;

    for (i = 0; i < 256; i++) {
        float f = (float) i / 255.0f;

        memcpy(&unorm8[i], &f, 4);
    }
    unorm8_ready = true;
}

// Row r gets (x, y, z, w) when it differs, and gives 1 then; words compare and copy without a trip through the FPU.
// The caller gathers the changed bits: a read-modify-write of changed[] per row waits for the last one's store.
static inline __attribute__((always_inline)) u32 row_words(float c[C_COUNT][4], u32 r, u32 x, u32 y, u32 z, u32 w)
{
    u32* d = (u32*) c[r];

    if (d[0] != x || d[1] != y || d[2] != z || d[3] != w) {
        d[0] = x;
        d[1] = y;
        d[2] = z;
        d[3] = w;
        return 1;
    }
    return 0;
}

// An RGBA8 colour as four floats 0-1. Values in [0, 1] compare as words as they do as floats.
static inline __attribute__((always_inline)) u32 row_rgba8(float c[C_COUNT][4], u32 r, u32 v)
{
    return row_words(c, r, unorm8[v >> 24], unorm8[(v >> 16) & 0xFF], unorm8[(v >> 8) & 0xFF], unorm8[v & 0xFF]);
}

// The float bits of k < 64, in integer registers: a conversion through the FPU stores and loads.
static u32 small_bits(u32 k)
{
    u32 e;

    if (k == 0) {
        return 0;
    }
    e = 31 - (u32) __builtin_clz(k);
    return (127 + e) << 23 | ((k << (23 - e)) & 0x7FFFFF);
}

#define F_ONE 0x3F800000u
#define F_MINUS_ONE 0xBF800000u
// 1e-30, safe_rcp's divisor for 0.
#define F_TINY 0x0DA24260u

// Rows C_LIGHT to C_MISC share one u64 of changed bits.
_Static_assert(C_MTX % 32 == 0 && C_NRM % 32 == 0 && C_POST % 32 == 0 && C_LIGHT % 32 == 0, "rows by word");
_Static_assert(C_MISC < C_LIGHT + 64, "C_LIGHT to C_MISC fit a u64");

bool xf_widescreen;

void xf_constants(const GpState* s, const u32* mem_dirty, const u32* reg_dirty, float c[C_COUNT][4], u32* changed)
{
    // The dirty words of position and texture matrices (XF 0x000-0x0FF) and of post-transform matrices
    // (0x500-0x5FF): group k of each is row k of the 32 rows at the matching row and changed word.
    static const u8 mtx_word[4] = { 0, 1, XF_DUALTEX_MTX / 128, XF_DUALTEX_MTX / 128 + 1 };
    static const u8 mtx_row[4] = { C_MTX, C_MTX + 32, C_POST, C_POST + 32 };
    const u32* x = s->xf_regs;
    const u32* m = s->xf_mem;
    u32 regs0 = reg_dirty != NULL ? reg_dirty[0] : ~0u, regs1 = reg_dirty != NULL ? reg_dirty[1] : ~0u;
    u32 bits, d, nrm = 0, lights = 0, i, k;
    u64 hi = 0;

    if (!unorm8_ready) {
        unorm8_init();
    }
    for (i = 0; i < 4; i++) {
        d = mem_dirty != NULL ? mem_dirty[mtx_word[i]] : ~0u;
        bits = 0;
        while (d != 0) {
            const u32* p;

            k = (u32) __builtin_ctz(d);
            d &= d - 1;
            p = &m[128 * mtx_word[i] + 4 * k];
            bits |= row_words(c, mtx_row[i] + k, p[0], p[1], p[2], p[3]) << k;
        }
        if (bits != 0) {
            changed[mtx_row[i] / 32] |= bits;
        }
    }
    // Normal matrices (XF 0x400-0x45F, groups 0-23 of their dirty word): each row of 3 words once, however many
    // dirty groups it straddles.
    d = (mem_dirty != NULL ? mem_dirty[XF_NRM_MTX / 128] : ~0u) & 0xFFFFFF;
    while (d != 0) {
        u32 lo, last;

        k = (u32) __builtin_ctz(d);
        d &= d - 1;
        lo = 4 * k / 3;
        last = (4 * k + 3) / 3;
        last = last > 31 ? 31 : last;
        nrm |= (2u << last) - (1u << lo);
    }
    bits = 0;
    while (nrm != 0) {
        const u32* n;

        k = (u32) __builtin_ctz(nrm);
        nrm &= nrm - 1;
        n = &m[XF_NRM_MTX + 3 * k];
        bits |= row_words(c, C_NRM + k, n[0], n[1], n[2], 0) << k;
    }
    if (bits != 0) {
        changed[C_NRM / 32] |= bits;
    }
    // Lights (XF 0x600-0x67F): 16 words, 4 groups each.
    d = mem_dirty != NULL ? mem_dirty[XF_LIGHTS / 128] : ~0u;
    for (i = 0; i < 8; i++) {
        lights |= ((d >> (4 * i)) & 0xF) != 0 ? 1u << i : 0;
    }
    for (i = 0; i < 8; i++) {
        const u32* l = &m[XF_LIGHTS + i * 16];
        u32 r = LIGHT_STRIDE * i;

        if (!((lights >> i) & 1)) {
            continue;
        }
        hi |= (u64) row_rgba8(c, C_LIGHT + r, l[3]) << r;
        hi |= (u64) row_words(c, C_LIGHT + r + 1, l[4], l[5], l[6], 0) << (r + 1);
        hi |= (u64) row_words(c, C_LIGHT + r + 2, l[7], l[8], l[9], 0) << (r + 2);
        hi |= (u64) row_words(c, C_LIGHT + r + 3, l[10], l[11], l[12], F_ONE) << (r + 3);
        hi |= (u64) row_words(c, C_LIGHT + r + 4, l[13], l[14], l[15], 0) << (r + 4);
    }
    if ((regs1 & XF_CONST_REGS_1) || (regs0 & XF_VIEWPORT_REGS)) {
        const u32* p = &x[0x20];

        // GX_ORTHOGRAPHIC in bit 0 of 0x26.
        if (x[0x26] & 1) {
            hi |= (u64) row_words(c, C_PROJ, p[0], 0, 0, p[1]) << (C_PROJ - C_LIGHT);
            hi |= (u64) row_words(c, C_PROJ + 1, 0, p[2], 0, p[3]) << (C_PROJ + 1 - C_LIGHT);
            hi |= (u64) row_words(c, C_PROJ + 3, 0, 0, 0, F_ONE) << (C_PROJ + 3 - C_LIGHT);
        } else if (xf_widescreen && x[0x1A] == 0x43A00000u) {
            // A viewport 320 pixels wide on each side of its centre: the EFB's, not a render to a texture.
            float sx, ox;
            u32 wx, wo;

            memcpy(&sx, &p[0], 4);
            memcpy(&ox, &p[1], 4);
            sx *= 0.75f;
            ox *= 0.75f;
            memcpy(&wx, &sx, 4);
            memcpy(&wo, &ox, 4);
            hi |= (u64) row_words(c, C_PROJ, wx, 0, wo, 0) << (C_PROJ - C_LIGHT);
            hi |= (u64) row_words(c, C_PROJ + 1, 0, p[2], p[3], 0) << (C_PROJ + 1 - C_LIGHT);
            hi |= (u64) row_words(c, C_PROJ + 3, 0, 0, F_MINUS_ONE, 0) << (C_PROJ + 3 - C_LIGHT);
        } else {
            hi |= (u64) row_words(c, C_PROJ, p[0], 0, p[1], 0) << (C_PROJ - C_LIGHT);
            hi |= (u64) row_words(c, C_PROJ + 1, 0, p[2], p[3], 0) << (C_PROJ + 1 - C_LIGHT);
            hi |= (u64) row_words(c, C_PROJ + 3, 0, 0, F_MINUS_ONE, 0) << (C_PROJ + 3 - C_LIGHT);
        }
        hi |= (u64) row_words(c, C_PROJ + 2, 0, 0, p[4], p[5]) << (C_PROJ + 2 - C_LIGHT);
    }
    if (regs0 & 0x3C00) {
        for (i = 0; i < 4; i++) {
            hi |= (u64) row_rgba8(c, C_CHAN + i, x[0x0A + i]) << (C_CHAN + i - C_LIGHT);
        }
    }
    if (regs0 & 0x03000000) {
        u32 a = x[0x18], b = x[0x19];

        hi |= (u64) row_words(c, C_MIDX, small_bits(a & 0x3F), small_bits((a >> 6) & 0x3F),
                              small_bits((a >> 12) & 0x3F), small_bits((a >> 18) & 0x3F))
              << (C_MIDX - C_LIGHT);
        hi |= (u64) row_words(c, C_MIDX + 1, small_bits((a >> 24) & 0x3F), small_bits(b & 0x3F),
                              small_bits((b >> 6) & 0x3F), small_bits((b >> 12) & 0x3F))
              << (C_MIDX + 1 - C_LIGHT);
        hi |= (u64) row_words(c, C_MIDX + 2, small_bits((b >> 18) & 0x3F), F_TINY, 0, 0) << (C_MIDX + 2 - C_LIGHT);
    }
    if (mem_dirty == NULL) {
        // (0, 1, 0.5, 2)
        hi |= (u64) row_words(c, C_MISC, 0, F_ONE, 0x3F000000u, 0x40000000u) << (C_MISC - C_LIGHT);
    }
    if (hi != 0) {
        changed[C_LIGHT / 32] |= (u32) hi;
        changed[C_LIGHT / 32 + 1] |= (u32) (hi >> 32);
    }
}

// The reference, step by step as the program.

static float dot3(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float dot4(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static void norm3(float* v)
{
    float l = 1.0f / sqrtf(dot3(v, v));

    v[0] *= l;
    v[1] *= l;
    v[2] *= l;
}

static float sat(float x)
{
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

static float dqf(const float c[C_COUNT][4], int n)
{
    return c[C_DQ + n / 4][n % 4];
}

static const float* ref_vtx_color(const XfKey* key, const XfIn* in, int j)
{
    static const float white[4] = { 1, 1, 1, 1 };

    if (has(key, VA_COL0 + j)) {
        return in->a[VA_COL0 + j];
    }
    if (has(key, VA_COL0)) {
        return in->a[VA_COL0];
    }
    return white;
}

// As safe_rcp's divisor.
static float ref_divisor(float d)
{
    return d == 0.0f ? 1e-30f : d;
}

static float ref_light(const float c[C_COUNT][4], int i, const Chan* ch, const float* eye, const float* nrm)
{
    const float* l = c[C_LIGHT + LIGHT_STRIDE * i];
    float ld[3] = { l[12] - eye[0], l[13] - eye[1], l[14] - eye[2] }, attn = 1.0f;

    if (ch->attn == ATTN_SPOT) {
        float d2 = dot3(ld, ld), dist = sqrtf(d2), a, num, den;

        ld[0] /= dist;
        ld[1] /= dist;
        ld[2] /= dist;
        a = dot3(ld, l + 16);
        a = a > 0.0f ? a : 0.0f;
        num = l[4] + l[5] * a + l[6] * a * a;
        num = num > 0.0f ? num : 0.0f;
        den = l[8] + l[9] * dist + l[10] * d2;
        attn = num * (1.0f / ref_divisor(den));
    } else {
        norm3(ld);
        if (ch->attn == ATTN_SPEC) {
            float k[3] = { l[8], l[9], l[10] }, a, num, den;

            a = dot3(nrm, ld) >= 0.0f ? dot3(nrm, l + 16) : 0.0f;
            a = a > 0.0f ? a : 0.0f;
            if (ch->diff != DIFF_NONE) {
                norm3(k);
            }
            num = l[4] + l[5] * a + l[6] * a * a;
            num = num > 0.0f ? num : 0.0f;
            den = k[0] + k[1] * a + k[2] * a * a;
            attn = num * (1.0f / ref_divisor(den));
        }
    }
    if (ch->diff != DIFF_NONE) {
        float d = dot3(ld, nrm);

        if (ch->diff == DIFF_CLAMP && d < 0.0f) {
            d = 0.0f;
        }
        attn *= d;
    }
    return attn;
}

void xf_ref(const XfKey* key, const float c[C_COUNT][4], const XfIn* in, XfOut* out)
{
    float pos[4], eye[4], clip[4], nrm[3] = { 0, 0, 0 }, rawn[3] = { 0, 0, 0 }, col[2][4], midx[9];
    int i, j, k, slot;

    if (has(key, VA_MIDX_A)) {
        for (i = 0; i < 4; i++) {
            midx[i] = in->a[VA_MIDX_A][i];
        }
    } else {
        for (i = 0; i < 4; i++) {
            midx[i] = c[C_MIDX][i];
        }
    }
    for (i = 0; i < 4; i++) {
        midx[4 + i] = has(key, VA_MIDX_B) ? in->a[VA_MIDX_B][i] : c[C_MIDX + 1][i];
    }
    midx[8] = c[C_MIDX + 2][0];
    for (i = 0; i < 3; i++) {
        pos[i] = in->a[VA_POS][i] * dqf(c, 0);
    }
    if (!key->pos3) {
        pos[2] = 0.0f;
    }
    pos[3] = 1.0f;
    for (i = 0; i < 3; i++) {
        eye[i] = dot4(c[C_MTX + (int) midx[0] + i], pos);
    }
    eye[3] = 1.0f;
    for (i = 0; i < 4; i++) {
        clip[i] = dot4(c[C_PROJ + i], eye);
    }
    out->pos[0] = clip[0];
    out->pos[1] = clip[1];
    out->pos[2] = key->old_depth ? clip[2] + clip[3] : -clip[2];
    out->pos[3] = clip[3];
    if (has(key, VA_NRM)) {
        for (i = 0; i < 3; i++) {
            rawn[i] = in->a[VA_NRM][i] * dqf(c, 1);
        }
        for (i = 0; i < 3; i++) {
            nrm[i] = dot3(c[C_NRM + (int) midx[0] + i], rawn);
        }
        norm3(nrm);
    }
    for (j = 0; j < key->numchans && j < 2; j++) {
        Chan cc = decode_chan(key->chan[j]), ac = decode_chan(key->chan[2 + j]);
        const float* vc = ref_vtx_color(key, in, j);
        float mat[4], lacc[4];

        for (k = 0; k < 4; k++) {
            const Chan* ch = k < 3 ? &cc : &ac;

            mat[k] = ch->matvtx ? vc[k] : c[C_CHAN + 2 + j][k];
            lacc[k] = ch->enable ? (ch->ambvtx ? vc[k] : c[C_CHAN + j][k]) : 1.0f;
        }
        if (!cc.enable && !ac.enable) {
            memcpy(col[j], mat, sizeof(mat));
            continue;
        }
        for (i = 0; i < 8; i++) {
            const float* lc = c[C_LIGHT + LIGHT_STRIDE * i];

            if (cc.enable && ((cc.mask >> i) & 1)) {
                float f = ref_light(c, i, &cc, eye, nrm);

                for (k = 0; k < 3; k++) {
                    lacc[k] += f * lc[k];
                }
            }
            if (ac.enable && ((ac.mask >> i) & 1)) {
                lacc[3] += ref_light(c, i, &ac, eye, nrm) * lc[3];
            }
        }
        for (k = 0; k < 4; k++) {
            col[j][k] = mat[k] * sat(lacc[k]);
        }
    }
    if (key->numchans == 0) {
        memcpy(col[0], ref_vtx_color(key, in, 0), sizeof(col[0]));
        if (!has(key, VA_COL0)) {
            col[0][0] = col[0][1] = col[0][2] = col[0][3] = 1.0f;
        }
    }
    if (key->numchans < 2) {
        if (has(key, VA_COL1)) {
            memcpy(col[1], in->a[VA_COL1], sizeof(col[1]));
        } else {
            memcpy(col[1], col[0], sizeof(col[1]));
        }
    }
    memcpy(out->col, col, sizeof(col));
    for (i = 0; i < key->numtexgens && i < 8; i++) {
        u32 info = key->texinfo[i];
        int proj = (info >> 1) & 1, form = (info >> 2) & 1, type = (info >> 4) & 7, row = (info >> 7) & 0x1F;
        float src[4] = { 0, 0, 1, 1 }, tc[4] = { 0, 0, 1, 1 };

        if (type == 0) {
            int m = (int) midx[1 + i];

            if (row == 0) {
                memcpy(src, pos, sizeof(src));
            } else if (row == 1 || row == 3 || row == 4) {
                int a = row == 1 ? VA_NRM : row == 3 ? VA_BINRM : VA_TANGENT;

                for (k = 0; k < 3; k++) {
                    src[k] = has(key, a) ? in->a[a][k] * dqf(c, 1) : 0.0f;
                }
            } else if (row >= 5 && row <= 12 && has(key, VA_TEX0 + row - 5)) {
                src[0] = in->a[VA_TEX0 + row - 5][0] * dqf(c, 2 + row - 5);
                src[1] = in->a[VA_TEX0 + row - 5][1] * dqf(c, 2 + row - 5);
            }
            if (form == 0) {
                src[2] = 1.0f;
            }
            tc[0] = dot4(c[C_MTX + m], src);
            tc[1] = dot4(c[C_MTX + m + 1], src);
            tc[2] = proj ? dot4(c[C_MTX + m + 2], src) : 1.0f;
            if (key->dualtex) {
                u32 post = key->postinfo[i];
                int idx = post & 0x3F;
                float t[4];

                if ((post >> 8) & 1) {
                    norm3(tc);
                }
                tc[3] = 1.0f;
                for (k = 0; k < 3; k++) {
                    t[k] = dot4(c[C_POST + idx + k], tc);
                }
                memcpy(tc, t, sizeof(float) * 3);
            }
        } else if (type == 2 || type == 3) {
            tc[0] = col[type - 2][0];
            tc[1] = col[type - 2][1];
            tc[2] = 1.0f;
        } else {
            tc[0] = tc[1] = 0.0f;
            tc[2] = 1.0f;
        }
        out->tc[i][0] = tc[0];
        out->tc[i][1] = tc[1];
        out->tc[i][2] = 0.0f;
        out->tc[i][3] = proj ? tc[2] : 1.0f;
    }
    for (i = 0, slot = key->tev_first; i < XF_TEV_CONSTS; i++) {
        if ((key->tev_consts >> i) & 1) {
            memcpy(out->tc[slot], c[C_TEV + i], sizeof(float) * 4);
            slot++;
        }
    }
}

// A spread of XF states for the cgcomp check.
void xf_check_cases(VpProg* p, void (*add)(const char* name))
{
    static const struct {
        const char* name;
        u16 attrs;
        u8 pos3, numchans, numtexgens, dualtex;
        u16 tev;
        u32 chan[4];
        u32 texinfo;
        u32 post;
    } k[] = {
        { "xf unlit colour", 1u << VA_POS | 1u << VA_COL0, 1, 1, 0, 0, 0, { 1, 0, 1, 0 }, 0, 0 },
        { "xf textured", 1u << VA_POS | 1u << VA_TEX0, 1, 0, 1, 0, 0x01, { 0 }, 5u << 7, 0 },
        { "xf diffuse 2 lights", 1u << VA_POS | 1u << VA_NRM | 1u << VA_MIDX_A, 1, 1, 0, 0, 0,
          { 0x2 | 0x1C | 2u << 7 | 1u << 10, 0, 0x2 | 0x4 | 2u << 7 | 1u << 10, 0 }, 0, 0 },
        { "xf specular", 1u << VA_POS | 1u << VA_NRM | 1u << VA_COL0, 1, 2, 0, 0, 0,
          { 0x3 | 0x4 | 2u << 7 | 1u << 10, 0x2 | 0x8 | 1u << 9, 0x2 | 0x4 | 1u << 9 | 1u << 10, 0x41 }, 0, 0 },
        { "xf texgen nrm dualtex", 1u << VA_POS | 1u << VA_NRM | 1u << VA_TEX0, 0, 1, 2, 1, 0x3F,
          { 0x1, 0, 0x1, 0 }, 1u << 7 | 1u << 2 | 1u << 1, 0x103 },
    };
    XfKey key;
    unsigned n, i;

    vp_set_check(true);
    for (n = 0; n < sizeof(k) / sizeof(k[0]); n++) {
        memset(&key, 0, sizeof(key));
        key.attrs = k[n].attrs;
        key.pos3 = k[n].pos3;
        key.numchans = k[n].numchans;
        key.numtexgens = k[n].numtexgens;
        key.dualtex = k[n].dualtex;
        key.tev_first = key.numtexgens;
        key.tev_consts = k[n].tev;
        memcpy(key.chan, k[n].chan, sizeof(key.chan));
        for (i = 0; i < key.numtexgens; i++) {
            key.texinfo[i] = k[n].texinfo + (i << 7);
            key.postinfo[i] = k[n].post;
        }
        if (xf_gen(&key, p) < 0) {
            continue;
        }
        add(k[n].name);
    }
    vp_set_check(false);
}
