// Copyright 2026 Dolphin Emulator Project
// Licensed under GPLv2+; refer to the license.txt file included.

#include "tevgen.h"

#include <math.h>
#include <string.h>

// RSX temps: R1 PREV, R2-R4 REG0-REG2, R5 the stage's texel, R6-R8 inputs a b c, R9 input d, R10-R11 fog,
// R12 the z texture's depth, R13 an offset texcoord. R0 is the output.
enum { RT_TEX = 5, RT_A = 6, RT_B = 7, RT_C = 8, RT_D = 9, RT_F = 10, RT_G = 11, RT_Z = 12, RT_TC = 13 };

typedef struct Operand {
    bool is_const;
    FpSrc s;    // when not constant
    float v[4]; // when constant: only the lanes the combiner uses matter
} Operand;

static const float k_bias[3] = { 0.0f, 0.5f, -0.5f };
static const float k_scale[4] = { 1.0f, 2.0f, 4.0f, 0.5f };
// KCSEL/KASEL 0x00-0x07: fixed fractions, as Dolphin's PixelShaderGen tables.
static const int k_fixed[8] = { 255, 223, 191, 159, 128, 96, 64, 32 };

static FpSrc swizzled(FpSrc s, const u8 swap[4])
{
    int i;

    for (i = 0; i < 4; i++) {
        s.swz[i] = swap[i];
    }
    return s;
}

// One source component in all lanes.
static FpSrc replicated(FpSrc s, u8 component)
{
    int i;

    for (i = 0; i < 4; i++) {
        s.swz[i] = component;
    }
    return s;
}

static Operand constant(float x, float y, float z, float w)
{
    Operand o;

    memset(&o, 0, sizeof(o));
    o.is_const = true;
    o.v[0] = x;
    o.v[1] = y;
    o.v[2] = z;
    o.v[3] = w;
    return o;
}

static Operand source(FpSrc s)
{
    Operand o;

    memset(&o, 0, sizeof(o));
    o.s = s;
    return o;
}

static FpSrc ras(const TevStage* st)
{
    return fp_in(st->ras == RAS_COL1 ? FIN_COL1 : FIN_COL0, NULL);
}

static Operand konst_color(const TevConfig* cfg, const TevStage* st)
{
    int sel = st->kcsel, n, comp;

    if (sel < 8) {
        float f = k_fixed[sel] / 255.0f;
        return constant(f, f, f, 0.0f);
    }
    if (sel < 0x0C) {
        return constant(0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (sel < 0x10) {
        n = sel - 0x0C;
        if (cfg->kin[n] >= 0) {
            return source(fp_in(cfg->kin[n], NULL));
        }
        return constant(cfg->kval[n][0], cfg->kval[n][1], cfg->kval[n][2], 0.0f);
    }
    // 0x10-0x1F: one channel of K0-K3 in all lanes (r, g, b, a in groups of four).
    n = sel & 3;
    comp = (sel - 0x10) >> 2;
    if (cfg->kin[n] >= 0) {
        return source(replicated(fp_in(cfg->kin[n], NULL), (u8) comp));
    }
    return constant(cfg->kval[n][comp], cfg->kval[n][comp], cfg->kval[n][comp], 0.0f);
}

static Operand konst_alpha(const TevConfig* cfg, const TevStage* st)
{
    int sel = st->kasel, n, comp;

    if (sel < 8) {
        return constant(0.0f, 0.0f, 0.0f, k_fixed[sel] / 255.0f);
    }
    if (sel < 0x10) {
        return constant(0.0f, 0.0f, 0.0f, 0.0f);
    }
    n = sel & 3;
    comp = (sel - 0x10) >> 2;
    if (cfg->kin[n] >= 0) {
        return source(replicated(fp_in(cfg->kin[n], NULL), (u8) comp));
    }
    return constant(0.0f, 0.0f, 0.0f, cfg->kval[n][comp]);
}

static Operand color_in(const TevConfig* cfg, const TevStage* st, int sel)
{
    switch (sel) {
    case CC_CPREV:
        return source(fp_t(1, NULL));
    case CC_APREV:
        return source(fp_t(1, "w"));
    case CC_C0:
        return source(fp_t(2, NULL));
    case CC_A0:
        return source(fp_t(2, "w"));
    case CC_C1:
        return source(fp_t(3, NULL));
    case CC_A1:
        return source(fp_t(3, "w"));
    case CC_C2:
        return source(fp_t(4, NULL));
    case CC_A2:
        return source(fp_t(4, "w"));
    // A stage without a texture reads white, as Dolphin's TEV.
    case CC_TEXC:
        return st->tex < 0 ? constant(1, 1, 1, 1) : source(swizzled(fp_t(RT_TEX, NULL), st->tex_swap));
    case CC_TEXA:
        return st->tex < 0 ? constant(1, 1, 1, 1) : source(replicated(fp_t(RT_TEX, NULL), st->tex_swap[3]));
    case CC_RASC:
        return st->ras == RAS_ZERO ? constant(0, 0, 0, 0) : source(swizzled(ras(st), st->ras_swap));
    case CC_RASA:
        return st->ras == RAS_ZERO ? constant(0, 0, 0, 0) : source(replicated(ras(st), st->ras_swap[3]));
    case CC_ONE:
        return constant(1, 1, 1, 0);
    case CC_HALF:
        return constant(0.5f, 0.5f, 0.5f, 0);
    case CC_KONST:
        return konst_color(cfg, st);
    default:
        return constant(0, 0, 0, 0);
    }
}

static Operand alpha_in(const TevConfig* cfg, const TevStage* st, int sel)
{
    switch (sel) {
    case CA_APREV:
        return source(fp_t(1, "w"));
    case CA_A0:
        return source(fp_t(2, "w"));
    case CA_A1:
        return source(fp_t(3, "w"));
    case CA_A2:
        return source(fp_t(4, "w"));
    case CA_TEXA:
        return st->tex < 0 ? constant(1, 1, 1, 1) : source(replicated(fp_t(RT_TEX, NULL), st->tex_swap[3]));
    case CA_RASA:
        return st->ras == RAS_ZERO ? constant(0, 0, 0, 0) : source(replicated(ras(st), st->ras_swap[3]));
    case CA_KONST:
        return konst_alpha(cfg, st);
    default:
        return constant(0, 0, 0, 0);
    }
}

// Loads the colour operand into dst.xyz and the alpha operand into dst.w, in one MOV when they can share it.
static void load_operand(FpProg* p, int dst, Operand c, Operand a, bool sat)
{
    FpSrc z = fp_t(0, NULL);

    if (c.is_const && a.is_const) {
        int k = fp_const(p, c.v[0], c.v[1], c.v[2], a.v[3]);
        fp_op(p, FOP_MOV, dst, 0xF, false, fp_c(k, NULL), z, z);
        return;
    }
    if (!c.is_const && !a.is_const && c.s.type == a.s.type && c.s.index == a.s.index) {
        FpSrc s = c.s;

        s.swz[3] = a.s.swz[3];
        fp_op(p, FOP_MOV, dst, 0xF, sat, s, z, z);
        return;
    }
    if (c.is_const) {
        fp_op(p, FOP_MOV, dst, 0x7, false, fp_c(fp_const(p, c.v[0], c.v[1], c.v[2], 0.0f), NULL), z, z);
    } else {
        fp_op(p, FOP_MOV, dst, 0x7, sat, c.s, z, z);
    }
    if (a.is_const) {
        fp_op(p, FOP_MOV, dst, 0x8, false, fp_c(fp_const(p, 0.0f, 0.0f, 0.0f, a.v[3]), NULL), z, z);
    } else {
        fp_op(p, FOP_MOV, dst, 0x8, sat, a.s, z, z);
    }
}

static bool comb_same(const TevComb* x, const TevComb* y)
{
    return x->op == y->op && x->bias == y->bias && x->scale == y->scale && x->clamp == y->clamp;
}

// r = (d +- r + bias) * scale on the lanes of mask, saturated when clamped. r is in RT_B on entry.
static void finish(FpProg* p, const TevComb* cb, int mask)
{
    FpSrc z = fp_t(0, NULL), r = fp_t(RT_B, NULL);
    bool simple = cb->bias == TB_ZERO && cb->scale == TS_1;

    fp_op(p, FOP_ADD, RT_B, mask, simple && cb->clamp, fp_t(RT_D, NULL), cb->op ? fp_neg(r) : r, z);
    if (!simple) {
        float s = k_scale[cb->scale];
        int k = fp_const(p, s, k_bias[cb->bias] * s, 0.0f, 0.0f);

        fp_op(p, FOP_MAD, RT_B, mask, cb->clamp != 0, r, fp_c(k, "x"), fp_c(k, "y"));
    }
}

u32 tev_regs_read(const TevConfig* cfg)
{
    u32 written_c = 0, written_a = 0, read = 0;
    int i, k;

    for (i = 0; i < cfg->nstages; i++) {
        const TevStage* st = &cfg->st[i];
        const u8 csel[4] = { st->col.a, st->col.b, st->col.c, st->col.d };
        const u8 asel[4] = { st->alp.a, st->alp.b, st->alp.c, st->alp.d };

        for (k = 0; k < 4; k++) {
            if (csel[k] <= CC_A2) {
                u32 r = csel[k] >> 1;
                bool alpha = csel[k] & 1;

                if (!((alpha ? written_a : written_c) & (1u << r))) {
                    read |= 1u << r;
                }
            }
            if (asel[k] <= CA_A2 && !(written_a & (1u << asel[k]))) {
                read |= 1u << asel[k];
            }
        }
        written_c |= 1u << st->col.dest;
        written_a |= 1u << st->alp.dest;
    }
    // The output is PREV.
    if (!(written_c & written_a & 1)) {
        read |= 1;
    }
    return read;
}

u32 tev_konst_read(const TevConfig* cfg)
{
    u32 read = 0;
    int i;

    for (i = 0; i < cfg->nstages; i++) {
        const TevStage* st = &cfg->st[i];
        const u8 csel[4] = { st->col.a, st->col.b, st->col.c, st->col.d };
        const u8 asel[4] = { st->alp.a, st->alp.b, st->alp.c, st->alp.d };
        int k;

        for (k = 0; k < 4; k++) {
            if (csel[k] == CC_KONST && st->kcsel >= 0x0C) {
                read |= 1u << (st->kcsel < 0x10 ? st->kcsel - 0x0C : st->kcsel & 3);
            }
            if (asel[k] == CA_KONST && st->kasel >= 0x10) {
                read |= 1u << (st->kasel & 3);
            }
        }
    }
    return read;
}

// The fog factor into R10.x, then PREV.rgb toward the fog colour. As Dolphin's PixelShaderGen, in float.
// depth: the window depth in 0..1.
static void gen_fog(const TevConfig* cfg, FpProg* p, FpSrc depth)
{
    FpSrc z = fp_t(0, NULL), f = fp_t(RT_F, "x"), prm, col;
    int k24 = fp_const(p, 16777216.0f, 0.0f, 0.0f, 0.0f);

    if (cfg->fog_pin >= 0) {
        prm = fp_in(cfg->fog_pin, NULL);
    } else {
        prm = fp_c(fp_const(p, cfg->fog_param[0], cfg->fog_param[1], cfg->fog_param[2], cfg->fog_param[3]), NULL);
    }
    if (cfg->fog_cin >= 0) {
        col = fp_in(cfg->fog_cin, NULL);
    } else {
        col = fp_c(fp_const(p, cfg->fog_color[0], cfg->fog_color[1], cfg->fog_color[2], 0.0f), NULL);
    }
    // zs: the window depth as a 24-bit value.
    fp_op(p, FOP_MUL, RT_F, 1, false, depth, fp_c(k24, "x"), z);
    if (cfg->fog_ortho) {
        fp_op(p, FOP_MAD, RT_F, 1, false, f, replicated(prm, 0), fp_neg(replicated(prm, 3)));
    } else {
        // ze = A / (B - zs >> shift) - C
        fp_op(p, FOP_MAD, RT_F, 2, false, fp_neg(f), replicated(prm, 2), replicated(prm, 1));
        fp_op(p, FOP_RCP, RT_F, 2, false, fp_t(RT_F, "y"), z, z);
        fp_op(p, FOP_MAD, RT_F, 1, false, fp_t(RT_F, "y"), replicated(prm, 0), fp_neg(replicated(prm, 3)));
    }
    fp_op(p, FOP_MOV, RT_F, 1, true, f, z, z);
    if (cfg->fog != FOG_LINEAR) {
        int k = fp_const(p, -8.0f, 1.0f, 0.0f, 0.0f);
        bool rev = cfg->fog == FOG_REV_EXP || cfg->fog == FOG_REV_EXP2;
        bool sq = cfg->fog == FOG_EXP2 || cfg->fog == FOG_REV_EXP2;

        if (rev) {
            fp_op(p, FOP_ADD, RT_F, 1, false, fp_neg(f), fp_c(k, "y"), z);
        }
        if (sq) {
            fp_op(p, FOP_MUL, RT_F, 1, false, f, f, z);
        }
        fp_op(p, FOP_MUL, RT_F, 1, false, f, fp_c(k, "x"), z);
        fp_op(p, FOP_EX2, RT_F, 1, false, f, z, z);
        if (!rev) {
            fp_op(p, FOP_ADD, RT_F, 1, false, fp_neg(f), fp_c(k, "y"), z);
        }
    }
    fp_op(p, FOP_ADD, RT_G, 7, false, col, fp_neg(fp_t(1, NULL)), z);
    fp_op(p, FOP_MAD, 1, 7, false, fp_t(RT_G, NULL), f, fp_t(1, NULL));
}

// The depth from the last texel sampled, unswapped, as Dolphin's PixelShaderGen: its bytes weighted by the type,
// plus the bias (and the fragment's depth for ADD), into R12.x in 0..1. Fog then reads it.
static void gen_ztex(const TevConfig* cfg, FpProg* p)
{
    static const float weights[3][4] = { { 0, 0, 0, 1 }, { 1, 0, 0, 256 }, { 65536, 256, 1, 0 } };
    const float* w = weights[cfg->ztex_type > ZTT_U24 ? ZTT_U24 : cfg->ztex_type];
    FpSrc z = fp_t(0, NULL), t = fp_t(RT_Z, NULL);
    int kw = fp_const(p, w[0], w[1], w[2], w[3]);
    int k = fp_const(p, 255.0f, 0.5f, 16777215.0f, 1.0f / 16777215.0f);
    int kb = fp_const(p, cfg->ztex_bias / 16777215.0f, 0.0f, 0.0f, 0.0f);
    bool sampled = false;
    int i;

    for (i = 0; i < cfg->nstages; i++) {
        sampled |= cfg->st[i].tex >= 0;
    }
    if (sampled) {
        fp_op(p, FOP_MUL, RT_Z, 0xF, false, fp_t(RT_TEX, NULL), fp_c(k, "x"), z);
        // Whole bytes first: the weighted sum is then exact in float.
        fp_op(p, FOP_ADD, RT_Z, 0xF, false, t, fp_c(k, "y"), z);
        fp_op(p, FOP_FLR, RT_Z, 0xF, false, t, z, z);
        fp_op(p, FOP_DP4, RT_Z, 1, false, t, fp_c(kw, NULL), z);
    } else {
        fp_op(p, FOP_MOV, RT_Z, 1, false, fp_c(kb, "y"), z, z);
    }
    if (cfg->ztex_op == ZT_ADD) {
        fp_op(p, FOP_MAD, RT_Z, 1, false, fp_in(FIN_WPOS, "z"), fp_c(k, "z"), fp_t(RT_Z, "x"));
    }
    fp_op(p, FOP_MAD, RT_Z, 1, true, fp_t(RT_Z, "x"), fp_c(k, "w"), fp_c(kb, "x"));
    p->depth = true;
}

// The texel at the texcoord plus the GX offset, as Dolphin's GeometryShaderGen: across a point, s and t from 0 at
// the lower left to the offset at the upper right (the sprite's t runs down); across a line, s at the middle.
static void tex_offset(const TevConfig* cfg, FpProg* p, const TevStage* st)
{
    FpSrc z = fp_t(0, NULL), r = fp_t(RT_TC, NULL);

    // xyxy: only s and t are interpolated.
    if (cfg->tco_kind == TCO_POINT) {
        int k = fp_const(p, cfg->tco, -cfg->tco, 0.0f, cfg->tco);

        fp_op(p, FOP_MOV, RT_TC, 3, false, fp_in(FIN_SPRITE, "xyxy"), z, z);
        fp_op(p, FOP_MAD, RT_TC, 3, false, r, fp_c(k, "xyxy"), fp_c(k, "zwzw"));
        fp_op(p, FOP_ADD, RT_TC, 3, false, r, fp_in(FIN_TC0 + st->texcoord, "xyxy"), z);
    } else {
        int k = fp_const(p, cfg->tco * 0.5f, 0.0f, 0.0f, 0.0f);

        fp_op(p, FOP_MOV, RT_TC, 3, false, fp_in(FIN_TC0 + st->texcoord, "xyxy"), z, z);
        fp_op(p, FOP_ADD, RT_TC, 1, false, r, fp_c(k, "x"), z);
    }
    fp_tex_t(p, RT_TEX, RT_TC, st->tex);
}

int tev_gen(const TevConfig* cfg, FpProg* p)
{
    FpSrc z = fp_t(0, NULL);
    u32 read = tev_regs_read(cfg);
    bool ztex = cfg->ztex_op == ZT_ADD || cfg->ztex_op == ZT_REPLACE;
    int r, i, slots;

    fp_init(p);
    for (r = 0; r < 4; r++) {
        if (!(read & (1u << r))) {
            continue;
        }
        if (cfg->rin[r] >= 0) {
            fp_op(p, FOP_MOV, 1 + r, 0xF, false, fp_in(cfg->rin[r], NULL), z, z);
        } else {
            fp_op(p, FOP_MOV, 1 + r, 0xF, false,
                  fp_c(fp_const(p, cfg->reg[r][0], cfg->reg[r][1], cfg->reg[r][2], cfg->reg[r][3]), NULL), z, z);
        }
    }
    for (i = 0; i < cfg->nstages; i++) {
        const TevStage* st = &cfg->st[i];

        if (st->tex >= 0) {
            if (st->proj) {
                fp_txp(p, RT_TEX, st->texcoord, st->tex);
            } else if (cfg->tco_kind != TCO_NONE && ((cfg->tco_mask >> st->texcoord) & 1)) {
                tex_offset(cfg, p, st);
            } else {
                fp_tex(p, RT_TEX, st->texcoord, st->tex);
            }
        }
        load_operand(p, RT_A, color_in(cfg, st, st->col.a), alpha_in(cfg, st, st->alp.a), true);
        load_operand(p, RT_B, color_in(cfg, st, st->col.b), alpha_in(cfg, st, st->alp.b), true);
        load_operand(p, RT_C, color_in(cfg, st, st->col.c), alpha_in(cfg, st, st->alp.c), true);
        load_operand(p, RT_D, color_in(cfg, st, st->col.d), alpha_in(cfg, st, st->alp.d), false);
        // b - a, then a + (b - a) * c: the lerp of TEV's (1 - c) * a + c * b
        fp_op(p, FOP_ADD, RT_B, 0xF, false, fp_t(RT_B, NULL), fp_neg(fp_t(RT_A, NULL)), z);
        fp_op(p, FOP_MAD, RT_B, 0xF, false, fp_t(RT_B, NULL), fp_t(RT_C, NULL), fp_t(RT_A, NULL));
        if (comb_same(&st->col, &st->alp)) {
            finish(p, &st->col, 0xF);
        } else {
            finish(p, &st->col, 0x7);
            finish(p, &st->alp, 0x8);
        }
        if (st->col.dest == st->alp.dest) {
            fp_op(p, FOP_MOV, 1 + st->col.dest, 0xF, false, fp_t(RT_B, NULL), z, z);
        } else {
            fp_op(p, FOP_MOV, 1 + st->col.dest, 0x7, false, fp_t(RT_B, NULL), z, z);
            fp_op(p, FOP_MOV, 1 + st->alp.dest, 0x8, false, fp_t(RT_B, NULL), z, z);
        }
    }
    if (ztex) {
        gen_ztex(cfg, p);
    }
    if (cfg->fog != FOG_NONE) {
        gen_fog(cfg, p, ztex ? fp_t(RT_Z, "x") : fp_in(FIN_WPOS, "z"));
    }
    fp_op(p, FOP_MOV, FP_OUT, 0xF, false, fp_t(1, NULL), z, z);
    // The depth export register.
    if (ztex) {
        fp_op(p, FOP_MOV, 1, 4, false, fp_t(RT_Z, "x"), z, z);
    }
    slots = fp_end(p);
    return p->err ? -1 : slots;
}

static float sat(float x)
{
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

static float ref_konst(const TevConfig* cfg, int sel, int ch, bool alpha)
{
    if (sel < 8) {
        return k_fixed[sel] / 255.0f;
    }
    if (sel < 0x10) {
        return alpha || sel < 0x0C ? 0.0f : cfg->kval[sel - 0x0C][ch];
    }
    return cfg->kval[sel & 3][(sel - 0x10) >> 2];
}

void tev_ref(const TevConfig* cfg, const float ras[2][4], const float tex[8][4], float depth, float out[4])
{
    float R[4][4], T[4] = { 0, 0, 0, 0 };
    int i, ch, k;

    memcpy(R, cfg->reg, sizeof(R));
    for (i = 0; i < cfg->nstages; i++) {
        const TevStage* st = &cfg->st[i];
        const u8 csel[4] = { st->col.a, st->col.b, st->col.c, st->col.d };
        const u8 asel[4] = { st->alp.a, st->alp.b, st->alp.c, st->alp.d };
        float Ts[4], Rs[4] = { 0, 0, 0, 0 }, in[4][4], res[4];

        if (st->tex >= 0) {
            memcpy(T, tex[st->tex], sizeof(T));
        } else {
            T[0] = T[1] = T[2] = T[3] = 1.0f;
        }
        for (ch = 0; ch < 4; ch++) {
            Ts[ch] = T[st->tex_swap[ch]];
            if (st->ras != RAS_ZERO) {
                Rs[ch] = ras[st->ras][st->ras_swap[ch]];
            }
        }
        for (k = 0; k < 4; k++) {
            float v;

            for (ch = 0; ch < 3; ch++) {
                switch (csel[k]) {
                case CC_CPREV: v = R[0][ch]; break;
                case CC_APREV: v = R[0][3]; break;
                case CC_C0: v = R[1][ch]; break;
                case CC_A0: v = R[1][3]; break;
                case CC_C1: v = R[2][ch]; break;
                case CC_A1: v = R[2][3]; break;
                case CC_C2: v = R[3][ch]; break;
                case CC_A2: v = R[3][3]; break;
                case CC_TEXC: v = Ts[ch]; break;
                case CC_TEXA: v = Ts[3]; break;
                case CC_RASC: v = Rs[ch]; break;
                case CC_RASA: v = Rs[3]; break;
                case CC_ONE: v = 1.0f; break;
                case CC_HALF: v = 0.5f; break;
                case CC_KONST: v = ref_konst(cfg, st->kcsel, ch, false); break;
                default: v = 0.0f;
                }
                in[k][ch] = k < 3 ? sat(v) : v;
            }
            switch (asel[k]) {
            case CA_APREV: v = R[0][3]; break;
            case CA_A0: v = R[1][3]; break;
            case CA_A1: v = R[2][3]; break;
            case CA_A2: v = R[3][3]; break;
            case CA_TEXA: v = Ts[3]; break;
            case CA_RASA: v = Rs[3]; break;
            case CA_KONST: v = ref_konst(cfg, st->kasel, 3, true); break;
            default: v = 0.0f;
            }
            in[k][3] = k < 3 ? sat(v) : v;
        }
        for (ch = 0; ch < 4; ch++) {
            const TevComb* cb = ch < 3 ? &st->col : &st->alp;
            float t = in[0][ch] + (in[1][ch] - in[0][ch]) * in[2][ch];
            float r = cb->op ? in[3][ch] - t : in[3][ch] + t;

            r = (r + k_bias[cb->bias]) * k_scale[cb->scale];
            res[ch] = cb->clamp ? sat(r) : r;
        }
        for (ch = 0; ch < 3; ch++) {
            R[st->col.dest][ch] = res[ch];
        }
        R[st->alp.dest][3] = res[3];
    }
    if (cfg->fog != FOG_NONE) {
        const float* f = cfg->fog_param;
        float zs = depth * 16777216.0f, ze, fog;

        ze = cfg->fog_ortho ? zs * f[0] - f[3] : f[0] / (f[1] - zs * f[2]) - f[3];
        fog = sat(ze);
        switch (cfg->fog) {
        case FOG_EXP: fog = 1.0f - exp2f(-8.0f * fog); break;
        case FOG_EXP2: fog = 1.0f - exp2f(-8.0f * fog * fog); break;
        case FOG_REV_EXP: fog = exp2f(-8.0f * (1.0f - fog)); break;
        case FOG_REV_EXP2: fog = exp2f(-8.0f * (1.0f - fog) * (1.0f - fog)); break;
        default: break;
        }
        for (ch = 0; ch < 3; ch++) {
            R[0][ch] += (cfg->fog_color[ch] - R[0][ch]) * fog;
        }
    }
    memcpy(out, R[0], sizeof(float) * 4);
}

int copy_gen(const float colmat[28], bool depth, FpProg* p)
{
    FpSrc z = fp_t(0, NULL);
    int mask, half, inv, add, j, slots;

    fp_init(p);
    fp_tex(p, 1, 0, 0);
    // round(t * mask) / mask: the bit depth of the destination format.
    mask = fp_const(p, colmat[20], colmat[21], colmat[22], colmat[23]);
    half = fp_const(p, 0.5f, 0.5f, 0.5f, 0.5f);
    inv = fp_const(p, colmat[24], colmat[25], colmat[26], colmat[27]);
    // A Z24S8 word read as A8R8G8B8 is (Z mid, Z low, stencil, Z high); Dolphin's depth texel is (high, mid, low, 1).
    fp_op(p, FOP_MUL, 1, 0xF, false, fp_t(1, depth ? "wxyz" : NULL), fp_c(mask, NULL), z);
    if (depth) {
        fp_op(p, FOP_MOV, 1, 8, false, fp_c(mask, "w"), z, z);
    }
    fp_op(p, FOP_ADD, 1, 0xF, false, fp_t(1, NULL), fp_c(half, NULL), z);
    fp_op(p, FOP_FLR, 1, 0xF, false, fp_t(1, NULL), z, z);
    fp_op(p, FOP_MUL, 1, 0xF, false, fp_t(1, NULL), fp_c(inv, NULL), z);
    // The row vector times the matrix whose columns are colmat[0..3], [4..7], [8..11], [12..15].
    for (j = 0; j < 4; j++) {
        int col = fp_const(p, colmat[4 * j], colmat[4 * j + 1], colmat[4 * j + 2], colmat[4 * j + 3]);
        fp_op(p, FOP_DP4, 2, 1 << j, false, fp_t(1, NULL), fp_c(col, NULL), z);
    }
    add = fp_const(p, colmat[16], colmat[17], colmat[18], colmat[19]);
    fp_op(p, FOP_ADD, FP_OUT, 0xF, false, fp_t(2, NULL), fp_c(add, NULL), z);
    slots = fp_end(p);
    return p->err ? -1 : slots;
}

int plain_gen(FpProg* p)
{
    FpSrc z = fp_t(0, NULL);
    int slots;

    fp_init(p);
    fp_tex(p, 1, 0, 0);
    fp_op(p, FOP_MOV, FP_OUT, 0xF, false, fp_t(1, NULL), z, z);
    slots = fp_end(p);
    return p->err ? -1 : slots;
}
