// Host test (qemu-ppc64): every SDK GX call that writes the pipe, with random arguments, into the ring. A thread
// takes the bytes unread; built with -DGX_PIPE_HASH, fifo.c prints their hash at exit, to compare two builds.
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dolphin/gx.h>
#include <dolphin/os.h>

#include "../gx/fifo.h"
#include "../os/intr.h"
#include "../os/mem.h"

#define ARENA_SIZE (8 << 20)
// Small, so the stream wraps it many times.
#define RING_SIZE 0x4000
#define ROUNDS 400

static u8* arena;
static u32 arena_used;
static u32 seed = 0x2545F491;

void OSReport(char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
    va_list ap;
    printf("OSPanic %s:%d: ", file, line);
    va_start(ap, msg);
    vprintf(msg, ap);
    va_end(ap);
    abort();
}

u32 VIGetTvFormat(void)
{
    return VI_NTSC;
}

OSThread* OSGetCurrentThread(void)
{
    return NULL;
}

s32 OSResumeThread(OSThread* thread)
{
    abort();
}

s32 OSSuspendThread(OSThread* thread)
{
    abort();
}

OSTime OSGetTime(void)
{
    abort();
}

static u32 rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static u32 rn(u32 n)
{
    return rnd() % n;
}

static f32 rf(void)
{
    return (f32) (s32) rnd() * (1.0f / 65536.0f);
}

static GXColor rcolor(void)
{
    u32 v = rnd();
    GXColor c = { v >> 24, v >> 16, v >> 8, v };

    return c;
}

static void* arena_alloc(u32 size)
{
    void* p = arena + arena_used;

    arena_used += (size + 31) & ~31u;
    return p;
}

// The GP thread's place: takes every published byte.
static void* consumer(void* arg)
{
    for (;;) {
        gp_fifo_take(1);
        gp_fifo_release_step();
    }
    return NULL;
}

static void rmtx34(f32 m[3][4])
{
    int i, j;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            m[i][j] = rf();
        }
    }
}

static void transforms(void)
{
    f32 m[3][4], n[3][3], p[4][4], v[7];
    int i, j;

    rmtx34(m);
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            n[i][j] = rf();
        }
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            p[i][j] = rf();
        }
    }
    GXLoadPosMtxImm(m, rn(10) * 3);
    GXLoadNrmMtxImm(m, rn(10) * 3);
    GXLoadNrmMtxImm3x3(n, rn(10) * 3);
    GXLoadTexMtxImm(m, GX_TEXMTX0 + rn(10) * 3, GX_MTX3x4);
    GXLoadTexMtxImm(m, GX_TEXMTX0 + rn(10) * 3, GX_MTX2x4);
    GXLoadTexMtxImm(m, GX_PTTEXMTX0 + rn(20) * 3, GX_MTX3x4);
    GXLoadPosMtxIndx(rn(0x10000), rn(10) * 3);
    GXLoadNrmMtxIndx3x3(rn(0x10000), rn(10) * 3);
    GXLoadTexMtxIndx(rn(0x10000), GX_TEXMTX0 + rn(10) * 3, rn(2) ? GX_MTX3x4 : GX_MTX2x4);
    GXSetCurrentMtx(rn(10) * 3);
    GXSetProjection(p, rn(2) ? GX_ORTHOGRAPHIC : GX_PERSPECTIVE);
    v[0] = (f32) rn(2);
    for (i = 1; i < 7; i++) {
        v[i] = rf();
    }
    GXSetProjectionv(v);
    GXSetViewport(rf(), rf(), rf(), rf(), rf(), rf());
    GXSetViewportJitter(rf(), rf(), rf(), rf(), rf(), rf(), rn(2));
    GXSetScissor(rn(1024), rn(1024), rn(1024), rn(1024));
    GXSetScissorBoxOffset((s32) rn(1024) - 512, (s32) rn(1024) - 512);
    GXSetClipMode(rn(2));
}

static void lights(void)
{
    GXLightObj l;

    GXInitLightColor(&l, rcolor());
    GXInitLightPos(&l, rf(), rf(), rf());
    GXInitLightDir(&l, rf(), rf(), rf());
    GXInitLightAttn(&l, rf(), rf(), rf(), rf(), rf(), rf());
    GXLoadLightObjImm(&l, 1 << rn(8));
    GXLoadLightObjIndx(rn(0x10000), 1 << rn(8));
    GXSetChanAmbColor(rn(6), rcolor());
    GXSetChanMatColor(rn(6), rcolor());
    GXSetNumChans(rn(3));
    GXSetChanCtrl(rn(6), rn(2), rn(2), rn(2), rn(256), rn(3), rn(3));
}

static void tev(void)
{
    GXColorS10 s = { (s16) (rn(2048) - 1024), (s16) (rn(2048) - 1024), (s16) (rn(2048) - 1024),
                     (s16) (rn(2048) - 1024) };
    static const u8 chans[] = { GX_COLOR0, GX_COLOR1, GX_ALPHA0, GX_ALPHA1, GX_COLOR0A0, GX_COLOR1A1, GX_COLOR_ZERO,
                                GX_ALPHA_BUMP, GX_ALPHA_BUMPN, GX_COLOR_NULL };
    u32 stage = rn(16), map = rn(9);

    GXSetTevOp(stage, rn(5));
    GXSetTevColorIn(stage, rn(16), rn(16), rn(16), rn(16));
    GXSetTevAlphaIn(stage, rn(8), rn(8), rn(8), rn(8));
    GXSetTevColorOp(stage, rn(16), rn(4), rn(4), rn(2), rn(4));
    GXSetTevAlphaOp(stage, rn(16), rn(4), rn(4), rn(2), rn(4));
    GXSetTevColor(rn(4), rcolor());
    GXSetTevColorS10(rn(4), s);
    GXSetTevKColor(rn(4), rcolor());
    GXSetTevKColorSel(stage, rn(32));
    GXSetTevKAlphaSel(stage, rn(32));
    GXSetTevSwapMode(stage, rn(4), rn(4));
    GXSetTevSwapModeTable(rn(4), rn(4), rn(4), rn(4), rn(4));
    GXSetAlphaCompare(rn(8), rn(256), rn(4), rn(8), rn(256));
    GXSetZTexture(rn(3), rn(2) ? GX_TF_Z8 : GX_TF_Z24X8, rn(1 << 24));
    GXSetTevOrder(stage, rn(2) ? GX_TEXCOORD_NULL : rn(8), map == 8 ? GX_TEXMAP_NULL : map | (rn(4) == 0) << 8,
                  chans[rn(sizeof(chans))]);
    GXSetNumTevStages(1 + rn(16));
    GXSetTevDirect(stage);
    GXSetTevIndirect(stage, rn(4), rn(4), rn(8), rn(12), rn(7), rn(7), rn(2), rn(2), rn(4));
    GXSetTevIndWarp(stage, rn(4), rn(2), rn(2), rn(4));
    GXSetIndTexCoordScale(rn(4), rn(9), rn(9));
    GXSetIndTexOrder(rn(4), rn(8), rn(8));
    GXSetNumIndStages(rn(5));
}

static void pixel(void)
{
    f32 z0 = (f32) rn(100), z1 = z0 + 1 + rn(1000), n = 0.25f + rn(10);

    GXSetFog(rn(8), z0, z1, n, n + 1 + rn(10000), rcolor());
    GXSetBlendMode(rn(4), rn(8), rn(8), rn(16));
    GXSetColorUpdate(rn(2));
    GXSetAlphaUpdate(rn(2));
    GXSetZMode(rn(2), rn(8), rn(2));
    GXSetZCompLoc(rn(2));
    GXSetPixelFmt(rn(8), rn(4));
    GXSetDither(rn(2));
    GXSetDstAlpha(rn(2), rn(256));
    GXSetFieldMask(rn(2), rn(2));
    GXSetFieldMode(rn(2), rn(2));
    GXPixModeSync();
    GXTexModeSync();
    GXSetLineWidth(rn(256), rn(8));
    GXSetPointSize(rn(256), rn(8));
    GXEnableTexOffsets(rn(8), rn(2), rn(2));
    GXSetCullMode(rn(4));
    GXSetCoPlanar(rn(2));
}

static void textures(void* image, void* lut)
{
    static const u8 fmts[] = { GX_TF_I4, GX_TF_I8, GX_TF_IA4, GX_TF_IA8, GX_TF_RGB565, GX_TF_RGB5A3, GX_TF_RGBA8,
                               GX_TF_CMPR };
    static const u8 ci[] = { GX_TF_C4, GX_TF_C8, GX_TF_C14X2 };
    GXTexObj obj;
    GXTlutObj tlut;
    u32 map = rn(8);

    GXInitTexObj(&obj, image, 1 + rn(1024), 1 + rn(1024), fmts[rn(sizeof(fmts))], rn(3), rn(3), rn(2));
    GXInitTexObjLOD(&obj, rn(6), rn(2), rn(48) * 0.25f, rn(48) * 0.25f, rn(16) * 0.25f, rn(2), rn(2), rn(3));
    GXLoadTexObj(&obj, map);
    GXInitTexObjCI(&obj, image, 1 + rn(1024), 1 + rn(1024), ci[rn(sizeof(ci))], rn(3), rn(3), rn(2), rn(16));
    GXLoadTexObj(&obj, map);
    GXInitTlutObj(&tlut, lut, rn(3), 16 * (1 + rn(16)));
    GXLoadTlut(&tlut, rn(16));
    GXSetTexCoordScaleManually(rn(8), rn(2), rn(0x10000), rn(0x10000));
    GXSetTexCoordCylWrap(rn(8), rn(2), rn(2));
    GXSetTexCoordBias(rn(8), rn(2), rn(2));
    if (rn(8) == 0) {
        GXInvalidateTexAll();
    }
}

static void texgens(void)
{
    u32 n = 1 + rn(8), i;

    for (i = 0; i < n; i++) {
        u32 func = rn(12), src;

        // Bump functions take a texcoord source, the others a position, normal or texcoord source.
        if (func >= GX_TG_BUMP0 && func <= GX_TG_BUMP7) {
            src = GX_TG_TEXCOORD0 + rn(7);
        } else if (func == GX_TG_SRTG) {
            src = GX_TG_COLOR0 + rn(2);
        } else {
            src = rn(12);
        }
        GXSetTexCoordGen2(i, func, src, GX_TEXMTX0 + rn(11) * 3, rn(2), GX_PTTEXMTX0 + rn(21) * 3);
    }
    GXSetNumTexGens(n);
}

static void copies(void* xfb)
{
    static const u8 pattern[12][2] = { { 6, 6 } };
    static const u8 vf[7] = { 0, 0, 21, 22, 21, 0, 0 };

    GXSetDispCopySrc(rn(640), rn(480), 1 + rn(640), 1 + rn(480));
    GXSetTexCopySrc(rn(640), rn(480), 1 + rn(640), 1 + rn(480));
    GXSetDispCopyDst(16 * (1 + rn(40)), 1 + rn(480));
    GXSetTexCopyDst(1 + rn(640), 1 + rn(480), rn(2) ? GX_TF_RGBA8 : GX_TF_I8, rn(2));
    GXSetCopyClear(rcolor(), rn(1 << 24));
    GXSetCopyFilter(rn(2), pattern, rn(2), vf);
    GXSetDispCopyGamma(rn(3));
    GXCopyDisp(xfb, rn(2));
    GXCopyTex(xfb, rn(2));
    GXClearBoundingBox();
}

static void arrays(void* data)
{
    u32 a;

    for (a = GX_VA_POS; a <= GX_LIGHT_ARRAY; a++) {
        GXSetArray(a, (u8*) data + 32 * rn(64), rn(256));
    }
    GXInvalidateVtxCache();
}

// Vertices of every GXVert.h store size and count, through every VAT; the GP's parse is not needed here.
static void vertices(void)
{
    u32 vat = rn(8), count = 1 + rn(20), i;

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    GXSetVtxDesc(GX_VA_POS, rn(3) + 1);
    GXSetVtxDesc(GX_VA_NRM, rn(4));
    GXSetVtxDesc(GX_VA_CLR0, rn(4));
    GXSetVtxDesc(GX_VA_TEX0 + rn(8), rn(4));
    GXSetVtxAttrFmt(vat, GX_VA_POS, rn(2), rn(5), rn(32));
    GXSetVtxAttrFmt(vat, GX_VA_NRM, rn(2), rn(5), 0);
    GXSetVtxAttrFmt(vat, GX_VA_CLR0, rn(2), rn(6), 0);
    GXSetVtxAttrFmt(vat, GX_VA_TEX0 + rn(8), rn(2), rn(5), rn(32));
    GXBegin(rn(8) << 3 | 0x80, vat, count);
    for (i = 0; i < count; i++) {
        GXMatrixIndex1u8(rn(256));
        GXPosition3f32(rf(), rf(), rf());
        GXPosition3u8(rnd(), rnd(), rnd());
        GXPosition3s8(rnd(), rnd(), rnd());
        GXPosition3u16(rnd(), rnd(), rnd());
        GXPosition3s16(rnd(), rnd(), rnd());
        GXPosition2f32(rf(), rf());
        GXPosition2u8(rnd(), rnd());
        GXPosition2s8(rnd(), rnd());
        GXPosition2u16(rnd(), rnd());
        GXPosition2s16(rnd(), rnd());
        GXPosition1x16(rnd());
        GXPosition1x8(rnd());
        GXNormal3f32(rf(), rf(), rf());
        GXNormal3s16(rnd(), rnd(), rnd());
        GXNormal3s8(rnd(), rnd(), rnd());
        GXNormal1x16(rnd());
        GXNormal1x8(rnd());
        GXColor4u8(rnd(), rnd(), rnd(), rnd());
        GXColor1u32(rnd());
        GXColor3u8(rnd(), rnd(), rnd());
        GXColor1u16(rnd());
        GXColor1x16(rnd());
        GXColor1x8(rnd());
        GXTexCoord2f32(rf(), rf());
        GXTexCoord2s16(rnd(), rnd());
        GXTexCoord2u16(rnd(), rnd());
        GXTexCoord2s8(rnd(), rnd());
        GXTexCoord2u8(rnd(), rnd());
        GXTexCoord1f32(rf());
        GXTexCoord1s16(rnd());
        GXTexCoord1u16(rnd());
        GXTexCoord1s8(rnd());
        GXTexCoord1u8(rnd());
        GXTexCoord1x16(rnd());
        GXTexCoord1x8(rnd());
        GXCmd1u8(rnd());
        GXCmd1u16(rnd());
        GXCmd1u32(rnd());
        GXParam1u8(rnd());
        GXParam1u16(rnd());
        GXParam1u32(rnd());
        GXParam1s8(rnd());
        GXParam1s16(rnd());
        GXParam1s32(rnd());
        GXParam1f32(rf());
        GXParam3f32(rf(), rf(), rf());
        GXParam4f32(rf(), rf(), rf(), rf());
    }
    GXEnd();
}

int main(void)
{
    pthread_t thread;
    void *fifo, *image, *lut, *xfb, *list, *data;
    u32 round;

    ps3_intr_init();
    arena = memalign(32, ARENA_SIZE);
    if (arena == NULL || !ps3_mem_add_region(arena, ARENA_SIZE)) {
        printf("FAIL: arena\n");
        return 1;
    }
    gp_fifo_init(RING_SIZE);
    pthread_create(&thread, NULL, consumer, NULL);
    fifo = arena_alloc(0x40000);
    image = arena_alloc(1 << 20);
    lut = arena_alloc(0x2000);
    xfb = arena_alloc(640 * 480 * 2);
    list = arena_alloc(0x1000);
    data = arena_alloc(0x1000);
    GXInit(fifo, 0x40000);
    for (round = 0; round < ROUNDS; round++) {
        transforms();
        lights();
        tev();
        pixel();
        textures(image, lut);
        texgens();
        arrays(data);
        vertices();
        GXCallDisplayList((u8*) list + 32 * rn(64), 32 * (1 + rn(64)));
        if (round % 50 == 0) {
            copies(xfb);
            GXSetGPMetric(rn(35), rn(23));
            GXClearGPMetric();
        }
        GXFlush();
    }
    gp_fifo_sync();
    printf("PASS (%u rounds)\n", ROUNDS);
    return 0;
}
