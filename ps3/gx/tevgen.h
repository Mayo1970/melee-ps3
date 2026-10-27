// Copyright 2026 Dolphin Emulator Project
// Licensed under GPLv2+; refer to the license.txt file included.

// GameCube TEV as an RSX fragment program, from DolphinPS3's TevGen: float math, as Dolphin's old float TEV.
// The konst colours, register start values and fog values arrive as interpolants, so they are not part of the code.
#ifndef PS3_GX_TEVGEN_H
#define PS3_GX_TEVGEN_H

#include "fpgen.h"

// Colour inputs, GX_CC_*.
enum {
    CC_CPREV, CC_APREV, CC_C0, CC_A0, CC_C1, CC_A1, CC_C2, CC_A2, CC_TEXC, CC_TEXA, CC_RASC, CC_RASA, CC_ONE,
    CC_HALF, CC_KONST, CC_ZERO
};
// Alpha inputs, GX_CA_*.
enum { CA_APREV, CA_A0, CA_A1, CA_A2, CA_TEXA, CA_RASA, CA_KONST, CA_ZERO };
enum { TB_ZERO, TB_ADDHALF, TB_SUBHALF };
enum { TS_1, TS_2, TS_4, TS_HALF };
enum { RAS_COL0, RAS_COL1, RAS_ZERO };
// GX fog types (BP fog param 3 fsel).
enum { FOG_NONE = 0, FOG_LINEAR = 2, FOG_EXP = 4, FOG_EXP2 = 5, FOG_REV_EXP = 6, FOG_REV_EXP2 = 7 };

typedef struct TevComb {
    u8 a, b, c, d, op, bias, scale, clamp, dest; // op: 0 add, 1 subtract; dest: PREV, REG0, REG1, REG2
} TevComb;

typedef struct TevStage {
    TevComb col, alp;
    s8 tex;      // texture unit, -1 = none
    u8 texcoord; // texcoord set of the lookup
    u8 proj;     // texcoord is projective (s, t, 0, q)
    u8 ras;      // RAS_*
    u8 ras_swap[4], tex_swap[4]; // source component of r, g, b, a
    u8 kcsel, kasel;             // GX_TEV_KCSEL_*, GX_TEV_KASEL_*
} TevStage;

// Cached by its bytes: clear it with memset before filling it. An input id below 0 means the value is in the
// code (kval, reg, fog_param, fog_color), used when the RSX has no free interpolant for it.
typedef struct TevConfig {
    u8 nstages;
    u8 fog;       // FOG_*
    u8 fog_ortho; // orthographic projection
    s8 kin[4];    // FP input of K0-K3, as (r, g, b, a)
    s8 rin[4];    // FP input of the start value of PREV, REG0, REG1, REG2
    s8 fog_pin;   // FP input of (A * 2^24, B magnitude, 2^-B shift, C), or of (A / 2^24, 0, 0, C) when orthographic
    s8 fog_cin;   // FP input of the fog colour
    TevStage st[16];
    float kval[4][4];
    float reg[4][4];
    float fog_param[4];
    float fog_color[4];
} TevConfig;

// The TEV registers a config reads before any stage writes them: bit r for PREV, REG0, REG1, REG2.
u32 tev_regs_read(const TevConfig* cfg);
// The konst colours it reads: bit n for Kn.
u32 tev_konst_read(const TevConfig* cfg);
// Builds the fragment program; returns its slot count, or -1.
int tev_gen(const TevConfig* cfg, FpProg* p);
// CPU reference for one pixel: ras[2] = vertex colours, tex[unit] = the texel each unit returns, depth = window
// z in 0..1. kval, reg and the fog values are always taken from the config here.
void tev_ref(const TevConfig* cfg, const float ras[2][4], const float tex[8][4], float depth, float out[4]);

// EFB copy colour matrix: out = round(t * mask) / mask * M + add (colmat[28], as Dolphin's TextureCacheBase).
int copy_gen(const float colmat[28], FpProg* p);
// The texel as it is, from texcoord 0 on unit 0.
int plain_gen(FpProg* p);

#endif
