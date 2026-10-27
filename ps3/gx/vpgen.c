// NV40/RSX vertex program builder. See vpgen.h.
#include "vpgen.h"

#include <stdio.h>
#include <string.h>

// Instruction word 0.
#define I0_VEC_RESULT (1u << 30)
#define I0_SATURATE (1u << 26)
#define I0_VEC_DEST_SHIFT 15
#define I0_VEC_DEST_NONE (0x3Fu << 15)
// Condition TR with the identity condition swizzle, as cgcomp writes every unconditional instruction.
#define I0_COND_TR (7u << 10 | 0u << 8 | 1u << 6 | 2u << 4 | 3u << 2)
// Word 1.
#define I1_VEC_OP_SHIFT 22
#define I1_SCA_OP_SHIFT 27
#define I1_CONST_SHIFT 12
#define I1_INPUT_SHIFT 8
// Word 3.
#define I3_SCA_MASK_SHIFT 17
#define I3_VEC_MASK_SHIFT 13
#define I3_SCA_RESULT (1u << 12)
#define I3_SCA_DEST_SHIFT 7
#define I3_SCA_DEST_NONE (0x3Fu << 7)
#define I3_DEST_SHIFT 2
#define I3_DEST_NONE (0x1Fu << 2)
#define I3_INDEX_CONST (1u << 1)
#define I3_LAST 1u
// Source fields.
#define SRC_NEGATE (1u << 16)

static char s_body[VP_TEXT - 1024];
static int s_blen;
static bool s_text = true;
static bool s_check;

void vp_set_text(bool on)
{
    s_text = on;
}

void vp_set_check(bool on)
{
    s_check = on;
}

void vp_init(VpProg* p)
{
    memset(p, 0, sizeof(*p));
    p->max_temp = -1;
    s_blen = 0;
    s_body[0] = 0;
}

static VpSrc make(int type, int index, const char* swz)
{
    static const char k[] = "xyzw";
    VpSrc s;
    int i;

    memset(&s, 0, sizeof(s));
    s.type = (u8) type;
    s.index = (u8) index;
    for (i = 0; i < 4; i++) {
        char ch = swz && swz[0] ? swz[swz[1] ? i : 0] : k[i];
        s.swz[i] = (u8) (strchr(k, ch) - k);
    }
    return s;
}

VpSrc vp_t(int reg, const char* swz)
{
    return make(VS_TEMP, reg, swz);
}

VpSrc vp_in(int attr, const char* swz)
{
    return make(VS_INPUT, attr, swz);
}

VpSrc vp_c(int index, const char* swz)
{
    return make(VS_CONST, index, swz);
}

VpSrc vp_ca(int addr, int offset, const char* swz)
{
    VpSrc s = make(VS_CONST, offset, swz);

    s.relative = 1;
    s.addr = (u8) addr;
    return s;
}

VpSrc vp_neg(VpSrc s)
{
    s.neg ^= 1;
    return s;
}

VpSrc vp_abs(VpSrc s)
{
    s.abs = 1;
    return s;
}

// The cgcomp source slots of each operand: ADD reads slots 0 and 2, scalar ops slot 2.
static int slots_of(int op, int slots[3])
{
    if (op >= VOP_SCALAR) {
        slots[0] = 2;
        return 1;
    }
    switch (op) {
    case VOP_MOV:
    case VOP_ARL:
    case VOP_FRC:
    case VOP_FLR:
        slots[0] = 0;
        return 1;
    case VOP_ADD:
        slots[0] = 0;
        slots[1] = 2;
        return 2;
    case VOP_MAD:
        slots[0] = 0;
        slots[1] = 1;
        slots[2] = 2;
        return 3;
    default:
        slots[0] = 0;
        slots[1] = 1;
        return 2;
    }
}

// The NVFX write mask: x is bit 3.
static u32 hw_mask(int mask)
{
    return (mask & 1 ? 8u : 0) | (mask & 2 ? 4u : 0) | (mask & 4 ? 2u : 0) | (mask & 8 ? 1u : 0);
}

static void put_src(VpProg* p, u32* hw, const VpSrc* s, int pos, int* input, int* konst)
{
    u32 sr;

    if (s == NULL) {
        // An unused slot: an input with the identity swizzle, as cgcomp's NONE.
        sr = 2u | (0u << 14 | 1u << 12 | 2u << 10 | 3u << 8);
    } else {
        switch (s->type) {
        case VS_TEMP:
            sr = 1u | (u32) s->index << 2;
            break;
        case VS_INPUT:
            sr = 2u;
            if (*input >= 0 && *input != s->index) {
                p->err = 3;
            }
            *input = s->index;
            hw[1] |= (u32) s->index << I1_INPUT_SHIFT;
            p->input_mask |= 1u << s->index;
            break;
        default:
            sr = 3u;
            if (*konst >= 0 && *konst != s->index) {
                p->err = 4;
            }
            *konst = s->index;
            hw[1] |= (u32) s->index << I1_CONST_SHIFT;
            if (s->relative) {
                hw[3] |= I3_INDEX_CONST;
                hw[0] |= s->addr;
                p->rel[p->n] = 1;
            }
            break;
        }
        if (s->neg) {
            sr |= SRC_NEGATE;
        }
        if (s->abs) {
            hw[0] |= 1u << (21 + pos);
        }
        sr |= (u32) s->swz[0] << 14 | (u32) s->swz[1] << 12 | (u32) s->swz[2] << 10 | (u32) s->swz[3] << 8;
    }
    switch (pos) {
    case 0:
        hw[1] |= (sr & 0x1FE00) >> 9;
        hw[2] |= (sr & 0x1FF) << 23;
        break;
    case 1:
        hw[2] |= sr << 6;
        break;
    default:
        hw[2] |= (sr & 0x1F800) >> 11;
        hw[3] |= (sr & 0x7FF) << 21;
        break;
    }
}

static void output_mask(VpProg* p, int out)
{
    switch (out) {
    case VO_COL0:
        p->output_mask |= 0x5;
        break;
    case VO_COL1:
        p->output_mask |= 0xA;
        break;
    case VO_FOGC:
        p->output_mask |= 1u << 4;
        break;
    case VO_PSZ:
        p->output_mask |= 1u << 5;
        break;
    default:
        if (out >= VO_TC0) {
            p->output_mask |= 0x4000u << (out - VO_TC0);
        }
        break;
    }
}

static void emit(VpProg* p, int op, int dst, int mask, bool sat, const VpSrc* src, int nsrc)
{
    const VpSrc* at[3] = { NULL, NULL, NULL };
    int slots[3], input = -1, konst = -1, i;
    bool scalar = op >= VOP_SCALAR;
    u32* hw;

    if (p->n >= VP_MAXI) {
        p->err = 2;
        return;
    }
    hw = p->w[p->n];
    memset(hw, 0, 16);
    slots_of(op, slots);
    for (i = 0; i < nsrc; i++) {
        at[slots[i]] = &src[i];
    }
    hw[0] = I0_COND_TR;
    if (sat) {
        hw[0] |= I0_SATURATE;
    }
    if (!scalar) {
        hw[1] |= (u32) op << I1_VEC_OP_SHIFT;
        hw[3] |= I3_SCA_DEST_NONE | hw_mask(mask) << I3_VEC_MASK_SHIFT;
    } else {
        hw[1] |= (u32) (op - VOP_SCALAR) << I1_SCA_OP_SHIFT;
        hw[0] |= I0_VEC_DEST_NONE;
        hw[3] |= hw_mask(mask) << I3_SCA_MASK_SHIFT;
    }
    if (dst & 0x100) {
        int out = dst & 0xFF;

        output_mask(p, out);
        hw[3] |= (u32) out << I3_DEST_SHIFT;
        if (!scalar) {
            hw[0] |= I0_VEC_RESULT | I0_VEC_DEST_NONE;
        } else {
            hw[3] |= I3_SCA_RESULT | I3_SCA_DEST_NONE;
        }
    } else {
        // A temp, or the address register A0 (index 0) for ARL.
        int reg = dst == VP_A0 ? 0 : dst;

        if (dst != VP_A0 && reg > p->max_temp) {
            p->max_temp = reg;
        }
        hw[3] |= I3_DEST_NONE;
        if (!scalar) {
            hw[0] |= (u32) reg << I0_VEC_DEST_SHIFT;
        } else {
            hw[3] |= (u32) reg << I3_SCA_DEST_SHIFT;
        }
    }
    for (i = 0; i < 3; i++) {
        put_src(p, hw, at[i], i, &input, &konst);
    }
    p->n++;
}

static const char* op_name(int op)
{
    switch (op) {
    case VOP_MOV: return "MOV";
    case VOP_MUL: return "MUL";
    case VOP_ADD: return "ADD";
    case VOP_MAD: return "MAD";
    case VOP_DP3: return "DP3";
    case VOP_DP4: return "DP4";
    case VOP_MIN: return "MIN";
    case VOP_MAX: return "MAX";
    case VOP_SLT: return "SLT";
    case VOP_SGE: return "SGE";
    case VOP_ARL: return "ARL";
    case VOP_FRC: return "FRC";
    case VOP_FLR: return "FLR";
    case VOP_RCP: return "RCP";
    case VOP_RSQ: return "RSQ";
    case VOP_LG2: return "LG2";
    case VOP_EX2: return "EX2";
    default: return "?";
    }
}

static int dst_text(char* buf, int size, int dst, int mask)
{
    char m[8] = "";
    int len, i;

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
    if (dst == VP_A0) {
        return snprintf(buf, size, "A0%s", m);
    }
    if (!(dst & 0x100)) {
        return snprintf(buf, size, "R%d%s", dst, m);
    }
    switch (dst & 0xFF) {
    case VO_POS:
        len = snprintf(buf, size, "result.position%s", m);
        break;
    case VO_COL0:
        len = snprintf(buf, size, "result.color%s", m);
        break;
    case VO_COL1:
        len = snprintf(buf, size, "result.color.secondary%s", m);
        break;
    case VO_FOGC:
        len = snprintf(buf, size, "result.fogcoord%s", m);
        break;
    case VO_PSZ:
        len = snprintf(buf, size, "result.pointsize%s", m);
        break;
    default:
        len = snprintf(buf, size, "result.texcoord[%d]%s", (dst & 0xFF) - VO_TC0, m);
        break;
    }
    return len;
}

static void src_text(char* buf, int size, const VpSrc* s, bool scalar)
{
    char sw[8] = "", reg[32];
    int i;

    if (scalar) {
        sw[0] = '.';
        sw[1] = "xyzw"[s->swz[0]];
        sw[2] = 0;
    } else if (!(s->swz[0] == 0 && s->swz[1] == 1 && s->swz[2] == 2 && s->swz[3] == 3)) {
        sw[0] = '.';
        for (i = 0; i < 4; i++) {
            sw[1 + i] = "xyzw"[s->swz[i]];
        }
        sw[5] = 0;
    }
    if (s->type == VS_TEMP) {
        snprintf(reg, sizeof(reg), "R%d", s->index);
    } else if (s->type == VS_INPUT) {
        snprintf(reg, sizeof(reg), "vertex.attrib[%d]", s->index);
    } else if (s->relative && !s_check) {
        snprintf(reg, sizeof(reg), "c[A0.%c + %d]", "xyzw"[s->addr], s->index);
    } else {
        snprintf(reg, sizeof(reg), "c[%d]", s->index);
    }
    snprintf(buf, size, "%s%s%s%s%s", s->neg ? "-" : "", s->abs ? "|" : "", reg, s->abs ? "|" : "", sw);
}

void vp_op(VpProg* p, int op, int dst, int mask, bool sat, VpSrc a, VpSrc b, VpSrc c)
{
    VpSrc s[3];
    int slots[3], nsrc, len, i;
    char line[256], t[64];

    s[0] = a;
    s[1] = b;
    s[2] = c;
    nsrc = slots_of(op, slots);
    if (op >= VOP_SCALAR) {
        // cgcomp replicates a scalar source's one component.
        for (i = 1; i < 4; i++) {
            s[0].swz[i] = s[0].swz[0];
        }
    }
    emit(p, op, dst, mask, sat, s, nsrc);
    if (!s_text) {
        return;
    }
    len = snprintf(line, sizeof(line), "%s%s ", op_name(op), sat ? "_SAT" : "");
    len += dst_text(line + len, sizeof(line) - len, dst, mask);
    for (i = 0; i < nsrc; i++) {
        src_text(t, sizeof(t), &s[i], op >= VOP_SCALAR);
        len += snprintf(line + len, sizeof(line) - len, ", %s", t);
    }
    snprintf(line + len, sizeof(line) - len, ";\n");
    s_blen += snprintf(s_body + s_blen, sizeof(s_body) - s_blen, "%s", line);
}

int vp_end(VpProg* p)
{
    if (p->n == 0) {
        vp_op(p, VOP_MOV, VP_OUT(VO_POS), 0xF, false, vp_c(0, NULL), vp_t(0, NULL), vp_t(0, NULL));
    }
    p->w[p->n - 1][3] |= I3_LAST;
    if (s_text) {
        p->tlen = snprintf(p->text, sizeof(p->text), "!!ARBvp1.0\nOPTION NV_vertex_program3;\n%sEND\n", s_body);
    }
    return p->n;
}
