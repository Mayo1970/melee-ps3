#ifndef _DOLPHIN_GX_GXTEV_H_
#define _DOLPHIN_GX_GXTEV_H_

#include <dolphin/gx/GXEnum.h>

#ifdef __cplusplus
extern "C" {
#endif

void GXSetTevOp(GXTevStageID id, GXTevMode mode);
void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b,
                     GXTevColorArg c, GXTevColorArg d);
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b,
                     GXTevAlphaArg c, GXTevAlphaArg d);
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                     GXTevScale scale, GXBool clamp, GXTevRegID out_reg);
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                     GXTevScale scale, GXBool clamp, GXTevRegID out_reg);
void GXSetTevColor(GXTevRegID id, GXColor color);
void GXSetTevColorS10(GXTevRegID id, GXColorS10 color);
void GXSetTevKColor(GXTevKColorID id, GXColor color);
void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel);
void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel);
void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel,
                      GXTevSwapSel tex_sel);
void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red,
                           GXTevColorChan green, GXTevColorChan blue,
                           GXTevColorChan alpha);
void GXSetTevClampMode(int, int);
void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1,
                       u8 ref1);
void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias);
void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map,
                   GXChannelID color);
void GXSetNumTevStages(u8 nStages);

#ifdef TARGET_PS3
// A stage's colour and alpha ops and inputs, swap and konst selections,
// laid out as HSD_TevConf (sysdolphin/baselib/texp.h).
typedef struct GXTevStagePS3 {
    GXTevOp clr_op;
    GXTevColorArg clr_a;
    GXTevColorArg clr_b;
    GXTevColorArg clr_c;
    GXTevColorArg clr_d;
    GXTevScale clr_scale;
    GXTevBias clr_bias;
    u8 clr_clamp;
    GXTevRegID clr_out_reg;
    GXTevOp alpha_op;
    GXTevAlphaArg alpha_a;
    GXTevAlphaArg alpha_b;
    GXTevAlphaArg alpha_c;
    GXTevAlphaArg alpha_d;
    GXTevScale alpha_scale;
    GXTevBias alpha_bias;
    u8 alpha_clamp;
    GXTevRegID alpha_out_reg;
    GXTevClampMode mode;
    GXTevSwapSel ras_swap;
    GXTevSwapSel tex_swap;
    GXTevKColorSel kcsel;
    GXTevKAlphaSel kasel;
} GXTevStagePS3;

// The seven TEV stage calls in one, each register written once. No BP mask
// may be pending: it would hit the final value, not an intermediate one.
void GXSetTevStagePS3(GXTevStageID stage, const GXTevStagePS3* conf);
#endif

#ifdef __cplusplus
}
#endif

#endif
