// NV40/RSX vertex program builder: the ucode as the RSX reads it, and the same program as vp40 assembly for the PC
// check against cgcomp. Encoding as in PSL1GHT's cgcomp (compilervp.cpp, nv40_vertprog.h).
#ifndef PS3_GX_VPGEN_H
#define PS3_GX_VPGEN_H

#include <stdbool.h>

#include <dolphin/types.h>

enum { VS_TEMP, VS_INPUT, VS_CONST };
// Vector opcodes; VOP_SCALAR + n are the scalar ones.
enum {
    VOP_MOV = 0x01,
    VOP_MUL = 0x02,
    VOP_ADD = 0x03,
    VOP_MAD = 0x04,
    VOP_DP3 = 0x05,
    VOP_DP4 = 0x07,
    VOP_MIN = 0x09,
    VOP_MAX = 0x0A,
    VOP_SLT = 0x0B,
    VOP_SGE = 0x0C,
    VOP_ARL = 0x0D,
    VOP_FRC = 0x0E,
    VOP_FLR = 0x0F,
    VOP_SCALAR = 0x100,
    VOP_RCP = VOP_SCALAR + 0x02,
    VOP_RSQ = VOP_SCALAR + 0x04,
    VOP_LG2 = VOP_SCALAR + 0x0D,
    VOP_EX2 = VOP_SCALAR + 0x0E,
};
// Outputs, for vp_op's dst: VP_OUT(n). VP_A0 is the address register.
enum { VO_POS = 0, VO_COL0 = 1, VO_COL1 = 2, VO_FOGC = 5, VO_PSZ = 6, VO_TC0 = 7 };
#define VP_OUT(n) (0x100 | (n))
#define VP_A0 0x200

typedef struct VpSrc {
    u8 type, index;
    u8 swz[4];
    u8 neg, abs;
    // A constant at c[A0.<addr> + index].
    u8 relative, addr;
} VpSrc;

#define VP_MAXI 512
#define VP_TEXT 65536

typedef struct VpProg {
    u32 w[VP_MAXI][4];
    int n;
    int max_temp;
    u32 input_mask, output_mask;
    // Instructions with c[A0 + n]: cgcomp cannot assemble them, so the check text has c[n] there.
    u8 rel[VP_MAXI];
    int err;
    char text[VP_TEXT];
    int tlen;
} VpProg;

void vp_init(VpProg* p);
// Off: no vp40 text.
void vp_set_text(bool on);
// On: the text writes c[A0.x + n] as c[n], for the cgcomp check.
void vp_set_check(bool on);
VpSrc vp_t(int reg, const char* swz);
VpSrc vp_in(int attr, const char* swz);
VpSrc vp_c(int index, const char* swz);
// c[A0.<addr> + offset]; addr 0-3 is x-w.
VpSrc vp_ca(int addr, int offset, const char* swz);
VpSrc vp_neg(VpSrc s);
VpSrc vp_abs(VpSrc s);
// mask: bits x=1 y=2 z=4 w=8. Unused sources: vp_t(0, NULL). Scalar ops read a's first swizzle component.
void vp_op(VpProg* p, int op, int dst, int mask, bool sat, VpSrc a, VpSrc b, VpSrc c);
// Sets the end bit; returns the instruction count.
int vp_end(VpProg* p);

#endif
