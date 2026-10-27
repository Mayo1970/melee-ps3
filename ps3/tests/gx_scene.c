// A GX test frame of nine cells through the SDK's GX API. See gx_scene.h.
#include "gx_scene.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include <dolphin/gx.h>
#include <dolphin/os.h>

#define CELL_W 200
#define CELL_H 145
#define SPHERE_SEG 16

const GxSceneCell gx_scene_cells[9] = {
    { "colours", "a triangle, red, green and blue corners blended" },
    { "RGBA8, mipmaps",
      "left: red and white checkers; middle: red to the right, green downward, smooth; right: one flat colour, "
      "green, blue or yellow (a smaller mip level)" },
    { "I8 x konst", "a green gradient, dark at the left" },
    { "CI8, CMPR", "left: eight colour bars; right: four squares, red green blue yellow" },
    { "lighting", "a red sphere lit from the upper left, a white highlight at the upper left, dark lower right" },
    { "skinning", "a band of two colours (s16 positions), its right half bent upward by a second matrix" },
    { "depth, blend", "blue square, a red half-transparent square over it, a green square only beside the blue" },
    { "fog", "four white squares, more grey with distance" },
    { "copy, cull", "cell 1's triangle as a texture (RGB565); one small yellow triangle at the bottom left only" },
};

static u8* arena;
static u32 arena_size, arena_used;
static u8* xfb;
static void* tex_checker;
static void* tex_mip;
static void* tex_i8;
static void* tex_ci8;
static void* tlut_ci8;
static void* tex_cmpr;
static void* tex_copy;

static void* alloc(u32 size)
{
    void* p = arena + arena_used;

    arena_used += (size + 31) & ~31u;
    return arena_used <= arena_size ? p : NULL;
}

static void put16(u8* p, u16 v)
{
    p[0] = (u8) (v >> 8);
    p[1] = (u8) v;
}

// RGBA8 in 4x4 tiles: the AR pairs of a tile, then its GB pairs.
static void encode_rgba8(u8* dst, const u32* argb, u32 w, u32 h)
{
    u32 tx, ty, i;

    for (ty = 0; ty < h; ty += 4) {
        for (tx = 0; tx < w; tx += 4) {
            for (i = 0; i < 16; i++) {
                u32 c = argb[(ty + i / 4) * w + tx + i % 4];

                dst[2 * i] = (u8) (c >> 24);
                dst[2 * i + 1] = (u8) (c >> 16);
                dst[32 + 2 * i] = (u8) (c >> 8);
                dst[32 + 2 * i + 1] = (u8) c;
            }
            dst += 64;
        }
    }
}

// An 8-bit format in 8x4 tiles.
static void encode_8(u8* dst, const u8* v, u32 w, u32 h)
{
    u32 tx, ty, i;

    for (ty = 0; ty < h; ty += 4) {
        for (tx = 0; tx < w; tx += 8) {
            for (i = 0; i < 32; i++) {
                *dst++ = v[(ty + i / 8) * w + tx + i % 8];
            }
        }
    }
}

static u16 rgb565(u32 r, u32 g, u32 b)
{
    return (u16) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

static void make_textures(void)
{
    static u32 img[64 * 64];
    static const u32 levels[7] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF };
    static const u16 bars[8][3] = { { 255, 0, 0 },   { 255, 128, 0 }, { 255, 255, 0 }, { 0, 255, 0 },
                                    { 0, 255, 255 }, { 0, 0, 255 },   { 128, 0, 255 }, { 255, 255, 255 } };
    static const u32 quads[4] = { 0xF800, 0x07E0, 0x001F, 0xFFE0 };
    u8 v[64 * 16];
    u32 x, y, l, size, off;
    u8* p;

    tex_checker = alloc(32 * 32 * 4);
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            img[y * 32 + x] = ((x / 8 + y / 8) & 1) ? 0xFFFFFFFF : 0xFFFF0000;
        }
    }
    encode_rgba8(tex_checker, img, 32, 32);
    // 64x64 to 1x1: level 0 a red-green gradient, the others one colour each.
    for (l = 0, size = 0; l < 7; l++) {
        u32 s = 64 >> l, t = s < 4 ? 4 : s;
        size += t * t * 4;
    }
    tex_mip = alloc(size);
    for (l = 0, off = 0; l < 7; l++) {
        u32 s = 64 >> l, t = s < 4 ? 4 : s;

        for (y = 0; y < t * t; y++) {
            img[y] = l == 0 ? 0xFF000000 | (y % t) * 4 << 16 | (y / t) * 4 << 8 : levels[l];
        }
        encode_rgba8((u8*) tex_mip + off, img, t, t);
        off += t * t * 4;
    }
    tex_i8 = alloc(64 * 16);
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 64; x++) {
            v[y * 64 + x] = (u8) (x * 4);
        }
    }
    encode_8(tex_i8, v, 64, 16);
    tex_ci8 = alloc(16 * 16);
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            v[y * 16 + x] = (u8) (x / 2);
        }
    }
    encode_8(tex_ci8, v, 16, 16);
    tlut_ci8 = alloc(256 * 2);
    for (x = 0; x < 256; x++) {
        put16((u8*) tlut_ci8 + 2 * x, x < 8 ? rgb565(bars[x][0], bars[x][1], bars[x][2]) : 0);
    }
    // CMPR 16x16: one 8x8 tile per quarter... as four tiles, each of four sub-blocks of one colour.
    tex_cmpr = alloc(4 * 32);
    p = tex_cmpr;
    for (l = 0; l < 4; l++) {
        for (x = 0; x < 4; x++) {
            put16(p, (u16) quads[l]);
            put16(p + 2, 0);
            memset(p + 4, 0, 4);
            p += 8;
        }
    }
    // RGB565 in whole 4x4 tiles: 148 lines for 145.
    tex_copy = alloc(CELL_W * 148 * 2 + 64);
}

static void mtx_identity(f32 m[3][4])
{
    memset(m, 0, sizeof(f32) * 12);
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

static void ortho(f32 m[4][4], f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
    memset(m, 0, sizeof(f32) * 16);
    m[0][0] = 2.0f / (r - l);
    m[0][3] = -(r + l) / (r - l);
    m[1][1] = 2.0f / (t - b);
    m[1][3] = -(t + b) / (t - b);
    m[2][2] = -1.0f / (f - n);
    m[2][3] = -f / (f - n);
    m[3][3] = 1.0f;
}

static void perspective(f32 m[4][4], f32 fovy, f32 aspect, f32 n, f32 f)
{
    f32 cot = 1.0f / tanf(fovy * 0.5f * 3.14159265f / 180.0f);

    memset(m, 0, sizeof(f32) * 16);
    m[0][0] = cot / aspect;
    m[1][1] = cot;
    m[2][2] = -n / (f - n);
    m[2][3] = -(f * n) / (f - n);
    m[3][2] = -1.0f;
}

static void cell_rect(int i, u32* x, u32* y)
{
    *x = 10 + (u32) (i % 3) * 210;
    *y = 10 + (u32) (i / 3) * 155;
}

void gx_scene_cell_center(int i, u32* x, u32* y)
{
    cell_rect(i, x, y);
    *x += CELL_W / 2;
    *y += CELL_H / 2;
}

// Viewport, scissor and projection for cell i: orthographic over [0, 1] x [0, 1], or a 60 degree perspective.
static void cell(int i, bool persp)
{
    f32 p[4][4], m[3][4];
    u32 x, y;

    cell_rect(i, &x, &y);
    GXSetViewport((f32) x, (f32) y, CELL_W, CELL_H, 0.0f, 1.0f);
    GXSetScissor(x, y, CELL_W, CELL_H);
    if (persp) {
        perspective(p, 60.0f, (f32) CELL_W / CELL_H, 0.5f, 20.0f);
        GXSetProjection(p, GX_PERSPECTIVE);
    } else {
        ortho(p, 1.0f, 0.0f, 0.0f, 1.0f, 0.1f, 10.0f);
        GXSetProjection(p, GX_ORTHOGRAPHIC);
    }
    mtx_identity(m);
    GXLoadPosMtxImm(m, GX_PNMTX0);
    GXLoadNrmMtxImm(m, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 10.0f, (GXColor) { 0, 0, 0, 0 });
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}

// A textured stage 0 with texcoord 0 from TEX0 as it is.
static void textured(GXTevMode mode)
{
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, mode);
}

// Format 0: position, colour, texcoord, all direct.
static void fmt0(void)
{
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
}

static void quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 z, u32 rgba, f32 s)
{
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x0, y1, z);
    GXColor1u32(rgba);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(x1, y1, z);
    GXColor1u32(rgba);
    GXTexCoord2f32(s, 0.0f);
    GXPosition3f32(x1, y0, z);
    GXColor1u32(rgba);
    GXTexCoord2f32(s, s);
    GXPosition3f32(x0, y0, z);
    GXColor1u32(rgba);
    GXTexCoord2f32(0.0f, s);
    GXEnd();
}

static void draw_colours(void)
{
    cell(0, false);
    fmt0();
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition3f32(0.1f, 0.1f, -1.0f);
    GXColor1u32(0xFF0000FF);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(0.5f, 0.9f, -1.0f);
    GXColor1u32(0x0000FFFF);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(0.9f, 0.1f, -1.0f);
    GXColor1u32(0x00FF00FF);
    GXTexCoord2f32(0.0f, 0.0f);
    GXEnd();
}

static void draw_textures(void)
{
    GXTexObj obj;

    cell(1, false);
    fmt0();
    textured(GX_REPLACE);
    GXInitTexObj(&obj, tex_checker, 32, 32, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    quad(0.03f, 0.1f, 0.31f, 0.9f, -1.0f, 0xFFFFFFFF, 1.0f);
    // The swizzled 64x64 at about 1:1, so level 0 shows; then sixteen repeats, about 16 texels per pixel.
    GXInitTexObj(&obj, tex_mip, 64, 64, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_TRUE);
    GXInitTexObjLOD(&obj, GX_LIN_MIP_NEAR, GX_LINEAR, 0.0f, 6.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    quad(0.36f, 0.1f, 0.64f, 0.9f, -1.0f, 0xFFFFFFFF, 1.0f);
    quad(0.69f, 0.1f, 0.97f, 0.9f, -1.0f, 0xFFFFFFFF, 16.0f);
}

static void draw_konst(void)
{
    GXTexObj obj;

    cell(2, false);
    fmt0();
    textured(GX_REPLACE);
    GXSetTevKColor(GX_KCOLOR0, (GXColor) { 0, 255, 0, 255 });
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXInitTexObj(&obj, tex_i8, 64, 16, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    quad(0.05f, 0.3f, 0.95f, 0.7f, -1.0f, 0xFFFFFFFF, 1.0f);
}

static void draw_palette(void)
{
    GXTexObj obj;
    GXTlutObj tlut;

    cell(3, false);
    fmt0();
    textured(GX_REPLACE);
    GXInitTlutObj(&tlut, tlut_ci8, GX_TL_RGB565, 256);
    GXLoadTlut(&tlut, GX_TLUT0);
    GXInitTexObjCI(&obj, tex_ci8, 16, 16, GX_TF_C8, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
    GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    quad(0.05f, 0.1f, 0.45f, 0.9f, -1.0f, 0xFFFFFFFF, 1.0f);
    GXInitTexObj(&obj, tex_cmpr, 16, 16, GX_TF_CMPR, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    quad(0.55f, 0.1f, 0.95f, 0.9f, -1.0f, 0xFFFFFFFF, 1.0f);
}

static void sphere_vertex(f32 lat, f32 lon, f32 r, f32 cz)
{
    f32 x = cosf(lat) * cosf(lon), y = sinf(lat), z = cosf(lat) * sinf(lon);

    GXPosition3f32(x * r, y * r, z * r + cz);
    GXNormal3f32(x, y, z);
}

static void draw_lighting(u32 n)
{
    GXLightObj diffuse, spec;
    f32 m[3][4];
    int i, j;

    cell(4, true);
    // The sphere turns about y: the normal matrix turns with it, the lights stay.
    mtx_identity(m);
    m[0][0] = m[2][2] = cosf((f32) n * 0.05f);
    m[0][2] = sinf((f32) n * 0.05f);
    m[2][0] = -m[0][2];
    m[2][3] = -3.0f;
    GXLoadPosMtxImm(m, GX_PNMTX1);
    m[2][3] = 0.0f;
    GXLoadNrmMtxImm(m, GX_PNMTX1);
    GXSetCurrentMtx(GX_PNMTX1);
    GXInitLightPos(&diffuse, -20.0f, 20.0f, 10.0f);
    GXInitLightColor(&diffuse, (GXColor) { 255, 255, 255, 255 });
    GXInitLightAttn(&diffuse, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    GXInitLightDir(&diffuse, 0.0f, 0.0f, -1.0f);
    GXLoadLightObjImm(&diffuse, GX_LIGHT0);
    // The direction the light travels: from the upper left, in front.
    GXInitSpecularDir(&spec, 0.5f, -0.5f, -0.7f);
    GXInitLightColor(&spec, (GXColor) { 255, 255, 255, 255 });
    // GXInitLightShininess(16).
    GXInitLightAttn(&spec, 0.0f, 0.0f, 1.0f, 8.0f, 0.0f, -7.0f);
    GXLoadLightObjImm(&spec, GX_LIGHT1);
    GXSetNumChans(2);
    GXSetChanCtrl(GX_COLOR0A0, GX_TRUE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, (GXColor) { 220, 30, 30, 255 });
    GXSetChanAmbColor(GX_COLOR0A0, (GXColor) { 40, 10, 10, 255 });
    GXSetChanCtrl(GX_COLOR1A1, GX_TRUE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT1, GX_DF_NONE, GX_AF_SPEC);
    GXSetChanMatColor(GX_COLOR1A1, (GXColor) { 255, 255, 255, 255 });
    GXSetChanAmbColor(GX_COLOR1A1, (GXColor) { 0, 0, 0, 0 });
    GXSetNumTevStages(2);
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR1A1);
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_RASC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
    for (i = 0; i < SPHERE_SEG; i++) {
        f32 a0 = -1.5707963f + 3.14159265f * i / SPHERE_SEG, a1 = -1.5707963f + 3.14159265f * (i + 1) / SPHERE_SEG;

        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT2, 2 * (SPHERE_SEG * 2 + 1));
        for (j = 0; j <= SPHERE_SEG * 2; j++) {
            f32 lon = 3.14159265f * j / SPHERE_SEG;

            sphere_vertex(a1, lon, 1.0f, 0.0f);
            sphere_vertex(a0, lon, 1.0f, 0.0f);
        }
        GXEnd();
    }
}

static void draw_skinning(void)
{
    f32 m[3][4];
    int i;

    cell(5, false);
    mtx_identity(m);
    m[0][3] = 0.0f;
    GXLoadPosMtxImm(m, GX_PNMTX0);
    // Matrix 1: 30 degrees about z, around (0.5, 0.5).
    m[0][0] = m[1][1] = 0.8660254f;
    m[0][1] = -0.5f;
    m[1][0] = 0.5f;
    m[0][3] = 0.5f - 0.8660254f * 0.5f + 0.5f * 0.5f;
    m[1][3] = 0.5f - 0.5f * 0.5f - 0.8660254f * 0.5f;
    GXLoadPosMtxImm(m, GX_PNMTX2);
    // Format 1: s16 positions with 8 fraction bits.
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT1, 10);
    for (i = 0; i < 5; i++) {
        s16 x = (s16) (256 * (0.1f + 0.2f * i));
        u8 idx = i < 2 ? GX_PNMTX0 : GX_PNMTX2;
        u32 c = i < 2 ? 0xFFFF00FF : 0x00FFFFFF;

        GXMatrixIndex1u8(idx);
        GXPosition3s16(x, 154, -256);
        GXColor1u32(c);
        GXMatrixIndex1u8(idx);
        GXPosition3s16(x, 102, -256);
        GXColor1u32(c);
    }
    GXEnd();
}

static void draw_depth_blend(void)
{
    cell(6, false);
    fmt0();
    quad(0.1f, 0.3f, 0.6f, 0.8f, -2.0f, 0x0000FFFF, 1.0f);
    // Behind the blue square: only its part beside it shows.
    quad(0.4f, 0.1f, 0.9f, 0.5f, -3.0f, 0x00FF00FF, 1.0f);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    quad(0.3f, 0.5f, 0.8f, 0.95f, -1.0f, 0xFF000080, 1.0f);
}

static void draw_fog(void)
{
    static const f32 depth[4] = { -2.0f, -3.5f, -5.0f, -6.5f };
    int i;

    cell(7, true);
    fmt0();
    GXSetFog(GX_FOG_LIN, 2.0f, 7.0f, 0.5f, 20.0f, (GXColor) { 96, 96, 96, 255 });
    // The same size and spacing on the screen at each depth: tan(30) * aspect is the half-width at distance 1.
    for (i = 0; i < 4; i++) {
        f32 d = -depth[i], hw = 0.57735f * ((f32) CELL_W / CELL_H) * d, x = (-0.72f + 0.48f * i) * hw;
        f32 s = 0.18f * hw;

        quad(x - s, -s, x + s, s, depth[i], 0xFFFFFFFF, 1.0f);
    }
}

static void draw_copy_cull(void)
{
    GXTexObj obj;
    u32 x, y;

    // Cell 0 (the triangle) to an RGB565 texture.
    cell_rect(0, &x, &y);
    GXSetTexCopySrc((u16) x, (u16) y, CELL_W, CELL_H);
    GXSetTexCopyDst(CELL_W, CELL_H, GX_TF_RGB565, GX_FALSE);
    GXCopyTex(tex_copy, GX_FALSE);
    GXPixModeSync();
    cell(8, false);
    fmt0();
    textured(GX_REPLACE);
    GXInitTexObj(&obj, tex_copy, CELL_W, CELL_H, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    quad(0.25f, 0.25f, 0.95f, 0.95f, -1.0f, 0xFFFFFFFF, 1.0f);
    // Two triangles, back faces culled: GX front faces are clockwise on the screen.
    GXSetNumTexGens(0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetCullMode(GX_CULL_BACK);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 6);
    // Clockwise on the screen (y up here): top, right, bottom-left.
    GXPosition3f32(0.12f, 0.2f, -1.0f);
    GXColor1u32(0xFFFF00FF);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(0.2f, 0.05f, -1.0f);
    GXColor1u32(0xFFFF00FF);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(0.04f, 0.05f, -1.0f);
    GXColor1u32(0xFFFF00FF);
    GXTexCoord2f32(0.0f, 0.0f);
    // Counter-clockwise: this one must not show.
    GXPosition3f32(0.12f, 0.95f, -1.0f);
    GXColor1u32(0xFF00FFFF);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(0.04f, 0.8f, -1.0f);
    GXColor1u32(0xFF00FFFF);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(0.2f, 0.8f, -1.0f);
    GXColor1u32(0xFF00FFFF);
    GXTexCoord2f32(0.0f, 0.0f);
    GXEnd();
}

void gx_scene_init(u8* a, u32 size)
{
    arena = a;
    arena_size = size;
    arena_used = 0;
    xfb = alloc(640 * 480 * 2);
    make_textures();
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GXSetCopyClear((GXColor) { 24, 24, 48, 255 }, 0xFFFFFF);
    GXSetDispCopySrc(0, 0, 640, 480);
    GXSetDispCopyDst(640, 480);
    GXSetDispCopyYScale(1.0f);
    // Clears the EFB.
    GXCopyDisp(xfb, GX_TRUE);
}

u32 gx_scene_frame(u32 n)
{
    GXInvalidateTexAll();
    GXInvalidateVtxCache();
    draw_colours();
    draw_textures();
    draw_konst();
    draw_palette();
    draw_lighting(n);
    draw_skinning();
    draw_depth_blend();
    draw_fog();
    draw_copy_cull();
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXCopyDisp(xfb, GX_TRUE);
    GXDrawDone();
    return OSCachedToPhysical(xfb);
}
