// Copyright 2026 Dolphin Emulator Project
// Licensed under GPLv2+; refer to the license.txt file included.

#include "fpgen.h"

#include <stdio.h>
#include <string.h>

static char s_body[FP_TEXT - 2048];
static int s_blen;
static bool s_text = true;

void fp_set_text(bool on)
{
    s_text = on;
}

void fp_init(FpProg* p)
{
    memset(p, 0, sizeof(*p));
    s_blen = 0;
    s_body[0] = 0;
}

int fp_const(FpProg* p, float x, float y, float z, float w)
{
    int i;

    for (i = 0; i < p->nc; i++) {
        if (p->c[i][0] == x && p->c[i][1] == y && p->c[i][2] == z && p->c[i][3] == w) {
            return i;
        }
    }
    if (p->nc >= FP_MAXC) {
        p->err = 1;
        return 0;
    }
    p->c[p->nc][0] = x;
    p->c[p->nc][1] = y;
    p->c[p->nc][2] = z;
    p->c[p->nc][3] = w;
    return p->nc++;
}

static FpSrc make(int type, int index, const char* swz)
{
    static const char k[] = "xyzw";
    FpSrc s;
    int i;

    s.type = (u8) type;
    s.index = (u8) index;
    s.neg = 0;
    for (i = 0; i < 4; i++) {
        char ch = swz && swz[0] ? swz[swz[1] ? i : 0] : k[i];
        s.swz[i] = (u8) (strchr(k, ch) - k);
    }
    return s;
}

FpSrc fp_t(int reg, const char* swz)
{
    return make(FS_TEMP, reg, swz);
}

FpSrc fp_in(int id, const char* swz)
{
    return make(FS_INPUT, id, swz);
}

FpSrc fp_c(int id, const char* swz)
{
    return make(FS_CONST, id, swz);
}

FpSrc fp_neg(FpSrc s)
{
    s.neg ^= 1;
    return s;
}

static bool identity(const FpSrc* s)
{
    return s->swz[0] == 0 && s->swz[1] == 1 && s->swz[2] == 2 && s->swz[3] == 3;
}

static void put(const char* line)
{
    s_blen += snprintf(s_body + s_blen, sizeof(s_body) - s_blen, "%s", line);
}

static void src_text(char* buf, int size, const FpSrc* s)
{
    char sw[8] = "";
    const char* neg = s->neg ? "-" : "";
    int i;

    if (!identity(s)) {
        sw[0] = '.';
        for (i = 0; i < 4; i++) {
            sw[1 + i] = "xyzw"[s->swz[i]];
        }
        sw[5] = 0;
    }
    if (s->type == FS_TEMP) {
        snprintf(buf, size, "%sR%d%s", neg, s->index, sw);
    } else if (s->type == FS_CONST) {
        snprintf(buf, size, "%sc[%d]%s", neg, s->index, sw);
    } else if (s->index == FIN_WPOS) {
        snprintf(buf, size, "%sfragment.position%s", neg, sw);
    } else if (s->index == FIN_COL0) {
        snprintf(buf, size, "%sfragment.color.primary%s", neg, sw);
    } else if (s->index == FIN_COL1) {
        snprintf(buf, size, "%sfragment.color.secondary%s", neg, sw);
    } else if (s->index == FIN_FOGC) {
        snprintf(buf, size, "%sfragment.fogcoord%s", neg, sw);
    } else {
        snprintf(buf, size, "%sfragment.texcoord[%d]%s", neg, s->index - FIN_TC0, sw);
    }
}

static u32 src_bits(FpProg* p, const FpSrc* s)
{
    u32 b = (u32) s->type;

    if (s->type == FS_TEMP) {
        b |= (u32) s->index << 2;
        if (s->index > p->max_temp) {
            p->max_temp = s->index;
        }
    }
    b |= (u32) (s->swz[0] | s->swz[1] << 2 | s->swz[2] << 4 | s->swz[3] << 6) << 9;
    if (s->neg) {
        b |= 1u << 17;
    }
    return b;
}

// Texcoord reads that need more than s and t: the header asks the RSX to interpolate them all.
static bool wide_read(const FpSrc* s)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (s->swz[i] > 1) {
            return true;
        }
    }
    return false;
}

static void emit(FpProg* p, int op, int dst, int mask, bool sat, const FpSrc* src, int nsrc, int unit)
{
    static const FpSrc none = { FS_TEMP, 0, { 0, 1, 2, 3 }, 0 };
    int input = -1, konst = -1, i;
    bool tc = false;
    u32* w;

    if (p->n >= FP_MAXI) {
        p->err = 2;
        return;
    }
    w = p->w[p->n];
    w[0] = (u32) op << 24 | (u32) mask << 9 | (u32) (dst == FP_OUT ? 0 : dst) << 1 | (sat ? 1u : 0u) << 31;
    if (dst != FP_OUT && dst > p->max_temp) {
        p->max_temp = dst;
    }
    for (i = 0; i < 3; i++) {
        const FpSrc* s = i < nsrc ? &src[i] : &none;

        if (s->type == FS_INPUT) {
            // One interpolated input per instruction.
            if (input >= 0 && input != s->index) {
                p->err = 3;
            }
            input = s->index;
            if (s->index >= FIN_TC0) {
                tc = true;
                // The header enables interpolation per texcoord, also for one only read by arithmetic.
                p->texcoords |= (u16) (1u << (s->index - FIN_TC0));
                if (wide_read(s)) {
                    p->texcoords4 |= (u16) (1u << (s->index - FIN_TC0));
                }
            }
        }
        if (s->type == FS_CONST) {
            // One inline constant per instruction.
            if (konst >= 0 && konst != s->index) {
                p->err = 4;
            }
            konst = s->index;
        }
        w[1 + i] = src_bits(p, s);
    }
    if (input >= 0) {
        w[0] |= (u32) input << 13;
    }
    if (op == FOP_TEX || op == FOP_TXP) {
        w[0] |= (u32) unit << 17;
    }
    // Condition TR, identity condition swizzle.
    w[1] |= 7u << 18 | 0xE4u << 21;
    if (tc) {
        w[3] |= 0x7FCu << 19;
    }
    p->kc[p->n] = konst;
    p->n++;
}

static const char* op_name(int op)
{
    switch (op) {
    case FOP_MOV:
        return "MOVR";
    case FOP_MUL:
        return "MULR";
    case FOP_ADD:
        return "ADDR";
    case FOP_MAD:
        return "MADR";
    case FOP_DP3:
        return "DP3R";
    case FOP_DP4:
        return "DP4R";
    case FOP_MIN:
        return "MINR";
    case FOP_MAX:
        return "MAXR";
    case FOP_SLT:
        return "SLTR";
    case FOP_SGE:
        return "SGER";
    case FOP_FRC:
        return "FRCR";
    case FOP_FLR:
        return "FLRR";
    case FOP_RCP:
        return "RCPR";
    case FOP_EX2:
        return "EX2R";
    case FOP_LG2:
        return "LG2R";
    default:
        return "?";
    }
}

static int num_sources(int op)
{
    switch (op) {
    case FOP_MOV:
    case FOP_FRC:
    case FOP_FLR:
    case FOP_RCP:
    case FOP_EX2:
    case FOP_LG2:
        return 1;
    case FOP_MAD:
        return 3;
    default:
        return 2;
    }
}

void fp_op(FpProg* p, int op, int dst, int mask, bool sat, FpSrc a, FpSrc b, FpSrc c)
{
    FpSrc s[3];
    int nsrc = num_sources(op), len, i;
    char line[256], t[64], m[8] = "";

    s[0] = a;
    s[1] = b;
    s[2] = c;
    emit(p, op, dst, mask, sat, s, nsrc, 0);
    if (!s_text) {
        return;
    }
    if (mask != 0xF) {
        int k = 0;

        m[k++] = '.';
        for (i = 0; i < 4; i++) {
            if (mask & (1 << i)) {
                m[k++] = "xyzw"[i];
            }
        }
        m[k] = 0;
    }
    len = snprintf(line, sizeof(line), "%s%s ", op_name(op), sat ? "_SAT" : "");
    if (dst == FP_OUT) {
        len += snprintf(line + len, sizeof(line) - len, "oCol%s", m);
    } else {
        len += snprintf(line + len, sizeof(line) - len, "R%d%s", dst, m);
    }
    for (i = 0; i < nsrc; i++) {
        src_text(t, sizeof(t), &s[i]);
        len += snprintf(line + len, sizeof(line) - len, ", %s", t);
    }
    snprintf(line + len, sizeof(line) - len, ";\n");
    put(line);
}

static void tex_op(FpProg* p, int op, int dst, int texcoord, int unit)
{
    FpSrc s = fp_in(FIN_TC0 + texcoord, NULL);
    char line[96];

    emit(p, op, dst, 0xF, false, &s, 1, unit);
    p->texcoords |= (u16) (1u << texcoord);
    if (op == FOP_TXP) {
        p->texcoords4 |= (u16) (1u << texcoord);
    }
    if (!s_text) {
        return;
    }
    snprintf(line, sizeof(line), "%s R%d, fragment.texcoord[%d], texture[%d], 2D;\n", op == FOP_TXP ? "TXP" : "TEX",
             dst, texcoord, unit);
    put(line);
}

void fp_tex(FpProg* p, int dst, int texcoord, int unit)
{
    tex_op(p, FOP_TEX, dst, texcoord, unit);
}

void fp_txp(FpProg* p, int dst, int texcoord, int unit)
{
    tex_op(p, FOP_TXP, dst, texcoord, unit);
}

void fp_tex_t(FpProg* p, int dst, int src, int unit)
{
    FpSrc s = fp_t(src, NULL);
    char line[96];

    emit(p, FOP_TEX, dst, 0xF, false, &s, 1, unit);
    if (!s_text) {
        return;
    }
    snprintf(line, sizeof(line), "TEX R%d, R%d, texture[%d], 2D;\n", dst, src, unit);
    put(line);
}

int fp_end(FpProg* p)
{
    int slots, len, i;

    if (p->n) {
        p->w[p->n - 1][0] |= 1;
    }
    slots = p->n;
    for (i = 0; i < p->n; i++) {
        slots += p->kc[i] >= 0;
    }
    if (!s_text) {
        return slots;
    }
    len = snprintf(p->text, sizeof(p->text), "!!ARBfp1.0\nOPTION NV_fragment_program2;\n");
    for (i = 0; i < 16; i++) {
        if (p->texcoords & (1u << i)) {
            len += snprintf(p->text + len, sizeof(p->text) - len,
                            "#var float%d tc%d : $vin.TEXCOORD%d : TEX%d : %d : 1\n",
                            (p->texcoords4 & (1u << i)) ? 4 : 2, i, i, i, i + 1);
        }
    }
    // cgcomp takes constant values from cgc's "#const" comments and skips PARAM, whose long lines crash it.
    for (i = 0; i < p->nc; i++) {
        len += snprintf(p->text + len, sizeof(p->text) - len, "#const c[%d] = %.9g %.9g %.9g %.9g\n", i, p->c[i][0],
                        p->c[i][1], p->c[i][2], p->c[i][3]);
    }
    for (i = 1; i <= p->max_temp; i++) {
        len += snprintf(p->text + len, sizeof(p->text) - len, "TEMP R%d;\n", i);
    }
    len += snprintf(p->text + len, sizeof(p->text) - len, "OUTPUT oCol = result.color;\n%sEND\n", s_body);
    p->tlen = len;
    return slots;
}

static u32 half_swap(u32 v)
{
    return v << 16 | v >> 16;
}

int fp_ucode(const FpProg* p, u32* out, int max_words)
{
    int k = 0, i, j;

    for (i = 0; i < p->n; i++) {
        if (k + 8 > max_words) {
            return -1;
        }
        for (j = 0; j < 4; j++) {
            out[k++] = half_swap(p->w[i][j]);
        }
        if (p->kc[i] >= 0) {
            for (j = 0; j < 4; j++) {
                u32 v;

                memcpy(&v, &p->c[p->kc[i]][j], sizeof(v));
                out[k++] = half_swap(v);
            }
        }
    }
    return k;
}

int fp_num_regs(const FpProg* p)
{
    return p->max_temp + 1 < 2 ? 2 : p->max_temp + 1;
}
