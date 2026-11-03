// Copyright 2026 Dolphin Emulator Project
// Licensed under GPLv2+; refer to the license.txt file included.

// NV40/RSX fragment program builder, from DolphinPS3's FPGen. It writes the ucode as the RSX reads it, and the
// same program as fp40 assembly for the PC check against cgcomp. Encoding as in PSL1GHT's cgcomp.
#ifndef PS3_GX_FPGEN_H
#define PS3_GX_FPGEN_H

#include <stdbool.h>

#include <dolphin/types.h>

enum { FS_TEMP, FS_INPUT, FS_CONST };
// Input ids; texcoord n is FIN_TC0 + n.
enum { FIN_WPOS = 0, FIN_COL0 = 1, FIN_COL1 = 2, FIN_FOGC = 3, FIN_TC0 = 4 };
enum {
    FOP_MOV = 0x01,
    FOP_MUL = 0x02,
    FOP_ADD = 0x03,
    FOP_MAD = 0x04,
    FOP_DP3 = 0x05,
    FOP_DP4 = 0x06,
    FOP_MIN = 0x08,
    FOP_MAX = 0x09,
    FOP_SLT = 0x0A,
    FOP_SGE = 0x0B,
    FOP_FRC = 0x10,
    FOP_FLR = 0x11,
    FOP_TEX = 0x17,
    FOP_TXP = 0x18,
    FOP_RCP = 0x1A,
    FOP_EX2 = 0x1C,
    FOP_LG2 = 0x1D,
};

typedef struct FpSrc {
    u8 type, index; // temp register, input id, or constant vector id
    u8 swz[4];
    u8 neg;
} FpSrc;

#define FP_MAXI 512
#define FP_MAXC 64
#define FP_TEXT 32768
// The destination of the colour output (hardware R0).
#define FP_OUT 0xFF

typedef struct FpProg {
    u32 w[FP_MAXI][4];
    int kc[FP_MAXI]; // constant vector read by the instruction, or -1
    int n;
    float c[FP_MAXC][4];
    int nc;
    int max_temp;
    // Texcoords read, and those read with more than two components.
    u16 texcoords, texcoords4;
    // The program writes the depth: R1.z, which the RSX takes with depth export on.
    bool depth;
    int err;
    char text[FP_TEXT];
    int tlen;
} FpProg;

void fp_init(FpProg* p);
// Off: no fp40 text (only the PC check needs it).
void fp_set_text(bool on);
int fp_const(FpProg* p, float x, float y, float z, float w);
FpSrc fp_t(int reg, const char* swz);
FpSrc fp_in(int id, const char* swz);
FpSrc fp_c(int id, const char* swz);
FpSrc fp_neg(FpSrc s);
// mask: bits x=1 y=2 z=4 w=8. Unused sources: fp_t(0, NULL). Scalar ops (RCP, EX2, LG2) read a.x.
void fp_op(FpProg* p, int op, int dst, int mask, bool sat, FpSrc a, FpSrc b, FpSrc c);
void fp_tex(FpProg* p, int dst, int texcoord, int unit);
// TXP: the texture at texcoord.xy / texcoord.w.
void fp_txp(FpProg* p, int dst, int texcoord, int unit);
// The texture at temp src.xy.
void fp_tex_t(FpProg* p, int dst, int src, int unit);
// Sets the end bit; returns the number of 16-byte slots (instructions plus inline constants).
int fp_end(FpProg* p);
// The ucode as 32-bit words in RSX order (halfwords swapped), constants inline. Returns words written, or -1.
int fp_ucode(const FpProg* p, u32* out, int max_words);
int fp_num_regs(const FpProg* p);

#endif
