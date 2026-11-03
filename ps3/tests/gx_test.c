// Host test (qemu-ppc64) of the SDK's GX library over ps3/gx: the GP must see the library's shadow registers,
// the draws as written, display lists, EFB copies, textures, tokens and draw done.
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dolphin/gx.h>
#include <dolphin/os.h>

#include "../../libs/dolphin/src/dolphin/gx/__gx.h"
#include "../gx/fifo.h"
#include "../gx/gp.h"
#include "../os/intr.h"
#include "../os/mem.h"

#define ARENA_SIZE (16 << 20)
// Small, so the stream test wraps it many times and some draws are larger than the ring.
#define RING_SIZE 0x10000
#define TIMEOUT_SECONDS 120

static u8* arena;
static u32 arena_used;
static int failures;

static struct {
    u32 draws, vertices;
    u64 hash;
    int prim, vat;
    u32 count;
    u8 data[4096];
    u32 size;
    u32 efb_copies, tlut_loads, tex_invalidates, finishes;
} rec;

static volatile u32 token_seen;
static volatile int token_calls;
static volatile int draw_done_calls;

void OSReport(char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
    va_list ap;
    fprintf(stderr, "OSPanic %s:%d: ", file, line);
    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
    abort();
}

u32 VIGetTvFormat(void)
{
    return VI_NTSC;
}

// The OS layer's, once it exists. GX calls them only on FIFO overflow and GXAbortFrame.
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

static void check(int ok, const char* what, u32 got, u32 want)
{
    if (!ok) {
        failures++;
        printf("FAIL %s: got 0x%08x, want 0x%08x\n", what, got, want);
    }
}

static void check_eq(const char* what, u32 got, u32 want)
{
    check(got == want, what, got, want);
}

static void* arena_alloc(u32 size)
{
    void* p = arena + arena_used;

    arena_used += (size + 31) & ~31u;
    return p;
}

static u64 fnv(u64 h, const u8* p, u32 n)
{
    while (n-- != 0) {
        h = (h ^ *p++) * 0x100000001B3ull;
    }
    return h;
}

static void rec_draw(GpState* s, int prim, int vat, u32 count, const u8* data)
{
    u32 size = count * gp_vertex_size(s, vat);

    rec.draws++;
    rec.vertices += count;
    rec.hash = fnv(rec.hash, data, size);
    rec.prim = prim;
    rec.vat = vat;
    rec.count = count;
    rec.size = size;
    memcpy(rec.data, data, size < sizeof(rec.data) ? size : sizeof(rec.data));
}

static void rec_efb_copy(GpState* s)
{
    rec.efb_copies++;
}

static void rec_tlut_load(GpState* s)
{
    rec.tlut_loads++;
}

static void rec_tex_invalidate(GpState* s)
{
    rec.tex_invalidates++;
}

static void rec_finish(GpState* s)
{
    rec.finishes++;
}

static const GpBackend rec_backend = {
    rec_draw, rec_efb_copy, rec_tlut_load, rec_tex_invalidate, rec_finish,
};

static void* watchdog(void* arg)
{
    sleep(TIMEOUT_SECONDS);
    printf("FAIL: timeout after %d s\n", TIMEOUT_SECONDS);
    fflush(stdout);
    _exit(2);
    return NULL;
}

static u32 f2u(f32 f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

// A BP shadow in __GXData holds its register number in the top byte.
static void check_bp_shadow(const char* what, u32 shadow)
{
    GpState* s = gp_get_state();
    u32 reg = shadow >> 24;

    check_eq(what, s->bp[reg], shadow & 0xFFFFFF);
}

static void test_init_state(void)
{
    GpState* s = gp_get_state();
    char name[64];
    int i;

    // GXInit only shadows the VATs; GXBegin sends them (test_direct_draw).
    check_eq("VCD lo", s->vcd_lo, gx->vcdLo);
    check_eq("VCD hi", s->vcd_hi, gx->vcdHi);
    check_bp_shadow("genMode", gx->genMode);
    for (i = 0; i < 16; i++) {
        sprintf(name, "tevc %d", i);
        check_bp_shadow(name, gx->tevc[i]);
        sprintf(name, "teva %d", i);
        check_bp_shadow(name, gx->teva[i]);
    }
    for (i = 0; i < 8; i++) {
        sprintf(name, "tevKsel %d", i);
        check_bp_shadow(name, gx->tevKsel[i]);
        sprintf(name, "tref %d", i);
        check_bp_shadow(name, gx->tref[i]);
        sprintf(name, "suTs0 %d", i);
        check_bp_shadow(name, gx->suTs0[i]);
        sprintf(name, "suTs1 %d", i);
        check_bp_shadow(name, gx->suTs1[i]);
    }
    check_bp_shadow("iref", gx->iref);
    check_bp_shadow("suScis0", gx->suScis0);
    check_bp_shadow("suScis1", gx->suScis1);
    check_bp_shadow("cmode0", gx->cmode0);
    check_bp_shadow("cmode1", gx->cmode1);
    check_bp_shadow("zmode", gx->zmode);
    check_bp_shadow("lpSize", gx->lpSize);
    // XF: GXInit loads identity position, normal and texture matrices, and the viewport.
    check_eq("pos mtx 0 [0][0]", s->xf_mem[0], f2u(1.0f));
    check_eq("pos mtx 0 [1][1]", s->xf_mem[5], f2u(1.0f));
    check_eq("pos mtx 0 [2][3]", s->xf_mem[11], f2u(0.0f));
    check_eq("nrm mtx 0 [1][1]", s->xf_mem[XF_NRM_MTX + 4], f2u(1.0f));
    check_eq("viewport scale x", s->xf_regs[0x1A], f2u(gx->vpWd * 0.5f));
}

static void test_matrices_lights(void)
{
    f32 m[3][4], n[3][3];
    GXLightObj light;
    GXColor color = { 0x12, 0x34, 0x56, 0x78 };
    GpState* s;
    int i, j;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            m[i][j] = (f32) (i * 4 + j) + 0.5f;
        }
        for (j = 0; j < 3; j++) {
            n[i][j] = (f32) (i * 3 + j) - 7.25f;
        }
    }
    GXLoadPosMtxImm(m, GX_PNMTX3);
    GXLoadNrmMtxImm3x3(n, GX_PNMTX3);
    GXLoadTexMtxImm(m, GX_TEXMTX2, GX_MTX2x4);
    GXInitLightColor(&light, color);
    GXInitLightPos(&light, 1.0f, -2.0f, 3.5f);
    GXInitLightDir(&light, 0.0f, 1.0f, 0.0f);
    GXInitLightAttn(&light, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    GXLoadLightObjImm(&light, GX_LIGHT5);
    GXSetCurrentMtx(GX_PNMTX3);
    gp_fifo_sync();
    s = gp_get_state();
    for (i = 0; i < 12; i++) {
        // A matrix id is its row address: 4 words per row, 3 for normal matrices.
        check_eq("pos mtx 3", s->xf_mem[GX_PNMTX3 * 4 + i], f2u(m[i / 4][i % 4]));
        if (i < 8) {
            check_eq("tex mtx 2", s->xf_mem[GX_TEXMTX2 * 4 + i], f2u(m[i / 4][i % 4]));
        }
        if (i < 9) {
            check_eq("nrm mtx 3", s->xf_mem[XF_NRM_MTX + GX_PNMTX3 * 3 + i], f2u(n[i / 3][i % 3]));
        }
    }
    check_eq("light 5 colour", s->xf_mem[XF_LIGHTS + 5 * 16 + 3], 0x12345678);
    check_eq("light 5 pos y", s->xf_mem[XF_LIGHTS + 5 * 16 + 11], f2u(-2.0f));
    check_eq("light 5 pos z", s->xf_mem[XF_LIGHTS + 5 * 16 + 12], f2u(3.5f));
    check_eq("matindex A (CP)", s->matindex_a & 0x3F, GX_PNMTX3);
    check_eq("matindex A (XF)", s->xf_regs[0x18] & 0x3F, GX_PNMTX3);
}

static void test_direct_draw(void)
{
    static const u8 want[] = {
        0x3F, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44,
        0x01, 0x00, 0x00, 0xFF, 0xBF, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x20, 0x00, 0x00,
        0x55, 0x66, 0x77, 0x88, 0x00, 0x80, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x42, 0xC8, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x99, 0xAA, 0xBB, 0xCC, 0x7F, 0xFF, 0x80, 0x00,
    };

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S16, 8);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition3f32(1.0f, 2.0f, 3.0f);
    GXColor1u32(0x11223344);
    GXTexCoord2s16(0x100, 0xFF);
    GXPosition3f32(-1.0f, 0.0f, 10.0f);
    GXColor1u32(0x55667788);
    GXTexCoord2s16(0x80, 0x40);
    GXPosition3f32(0.0f, 100.0f, -2.0f);
    GXColor1u32(0x99AABBCC);
    GXTexCoord2s16(0x7FFF, -0x8000);
    GXEnd();
    gp_fifo_sync();
    check_eq("direct prim", rec.prim, GP_TRIANGLES);
    check_eq("direct count", rec.count, 3);
    check_eq("direct vertex size", gp_vertex_size(gp_get_state(), 0), 20);
    check_eq("VAT A 0", gp_get_state()->vat_a[0], gx->vatA[0]);
    check_eq("VAT B 0", gp_get_state()->vat_b[0], gx->vatB[0]);
    check_eq("VAT C 0", gp_get_state()->vat_c[0], gx->vatC[0]);
    check(rec.size == sizeof(want) && memcmp(rec.data, want, sizeof(want)) == 0, "direct data", rec.size,
          sizeof(want));
}

static void test_indexed_draw(void)
{
    s16* pos = arena_alloc(64 * 6);
    u8* colors = arena_alloc(64 * 4);
    u16 i;

    for (i = 0; i < 64 * 3; i++) {
        pos[i] = (s16) (i * 3 - 100);
    }
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_NRM, GX_INDEX8);
    GXSetVtxDesc(GX_VA_CLR0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_S16, 4);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 6);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetArray(GX_VA_POS, pos, 6);
    GXSetArray(GX_VA_CLR0, colors, 4);
    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT2, 4);
    for (i = 0; i < 4; i++) {
        GXMatrixIndex1u8(i * 3);
        GXPosition1x16(i + 60);
        GXNormal1x8(i);
        GXColor1x8(i * 2);
    }
    GXEnd();
    gp_fifo_sync();
    check_eq("indexed prim", rec.prim, GP_TRIANGLE_STRIP);
    check_eq("indexed vat", rec.vat, 2);
    check_eq("indexed vertex size", gp_vertex_size(gp_get_state(), 2), 5);
    check(rec.data[5] == 3 && rec.data[6] == 0 && rec.data[7] == 61 && rec.data[8] == 1 && rec.data[9] == 2,
          "indexed vertex 1", rec.data[7], 61);
    check(OSPhysicalToCached(gp_get_state()->array_base[0]) == pos, "position array", gp_get_state()->array_base[0], 0);
    check_eq("position stride", gp_get_state()->array_stride[0], 6);
    check(OSPhysicalToCached(gp_get_state()->array_base[2]) == colors, "colour array", gp_get_state()->array_base[2], 0);
}

static void test_display_list(void)
{
    u8* dl = arena_alloc(64);
    GpStats st0, st1;
    u32 n = 0, i;

    // VCD and VAT 0 as in test_direct_draw, with only the position.
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XY, GX_S8, 0);
    memset(dl, 0, 64);
    dl[n++] = 0xA0 | GX_VTXFMT1; // triangle fan
    dl[n++] = 0;
    dl[n++] = 5;
    for (i = 0; i < 10; i++) {
        dl[n++] = (u8) (i * 7);
    }
    // A BP write inside the list: the TEV konst register 3, RA half.
    dl[n++] = 0x61;
    dl[n++] = 0xE6;
    dl[n++] = 0x80;
    dl[n++] = 0x0A;
    dl[n++] = 0xBC;
    gp_get_stats(&st0);
    GXCallDisplayList(dl, 64);
    gp_fifo_sync();
    gp_get_stats(&st1);
    check_eq("display lists run", st1.display_lists - st0.display_lists, 1);
    check_eq("list prim", rec.prim, GP_TRIANGLE_FAN);
    check_eq("list count", rec.count, 5);
    check(rec.size == 10 && rec.data[9] == 63, "list data", rec.data[9], 63);
    check_eq("list konst 3", gp_get_state()->tev_konst[3][0], 0x800ABC);
}

static void test_tev_registers(void)
{
    GXColor c = { 0x10, 0x20, 0x30, 0x40 };
    GXColor k = { 0xA0, 0xB0, 0xC0, 0xD0 };
    GpState* s;

    GXSetTevColor(GX_TEVREG1, c);
    GXSetTevKColor(GX_KCOLOR1, k);
    gp_fifo_sync();
    s = gp_get_state();
    // RA half: alpha in bits 12-19, red in 0-7; BG half: green in 12-19, blue in 0-7.
    check_eq("tev reg 1 RA", s->tev_color[2][0] & 0xFFFFF, 0x40010);
    check_eq("tev reg 1 BG", s->tev_color[2][1] & 0xFFFFF, 0x20030);
    check_eq("konst 1 RA", s->tev_konst[1][0] & 0xFFFFF, 0xD00A0);
    check_eq("konst 1 BG", s->tev_konst[1][1] & 0xFFFFF, 0xB00C0);
}

static void test_depth_mode(void)
{
    u32 generation;

    GXSetZCompLoc(GX_TRUE);
    gp_fifo_sync();
    generation = gp_get_state()->tev_gen;
    GXSetZCompLoc(GX_FALSE);
    gp_fifo_sync();
    check_eq("late depth invalidates TEV", gp_get_state()->tev_gen, generation + 1);
    generation = gp_get_state()->tev_gen;
    GXSetZCompLoc(GX_TRUE);
    gp_fifo_sync();
    check_eq("early depth invalidates TEV", gp_get_state()->tev_gen, generation + 1);
    GXSetZCompLoc(GX_TRUE);
    gp_fifo_sync();
    check_eq("same depth keeps TEV", gp_get_state()->tev_gen, generation + 1);
}

static void test_textures_copies(void)
{
    u8* tex = arena_alloc(64 * 64 * 4);
    u8* lut = arena_alloc(256 * 2);
    u8* xfb = arena_alloc(640 * 480 * 2);
    GXTexObj obj;
    GXTlutObj tlut;
    GpState* s;

    GXInitTexObj(&obj, tex, 64, 64, GX_TF_RGBA8, GX_REPEAT, GX_CLAMP, GX_FALSE);
    GXLoadTexObj(&obj, GX_TEXMAP3);
    GXInitTlutObj(&tlut, lut, GX_TL_RGB565, 256);
    GXLoadTlut(&tlut, GX_TLUT2);
    GXInvalidateTexAll();
    GXSetDispCopySrc(0, 0, 640, 480);
    GXSetDispCopyDst(640, 480);
    GXCopyDisp(xfb, GX_TRUE);
    gp_fifo_sync();
    s = gp_get_state();
    check(OSPhysicalToCached((s->bp[0x97] & 0xFFFFFF) << 5) == tex, "texmap 3 image", s->bp[0x97], 0);
    check_eq("texmap 3 size", s->bp[0x8B] & 0xFFFFF, (63 << 10) | 63);
    check(OSPhysicalToCached((s->bp[0x64] & 0xFFFFFF) << 5) == lut, "TLUT address", s->bp[0x64], 0);
    check_eq("TLUT loads", rec.tlut_loads, 1);
    check(rec.tex_invalidates >= 1, "texture invalidates", rec.tex_invalidates, 1);
    check_eq("EFB copies", rec.efb_copies, 1);
    check(OSPhysicalToCached((s->bp[0x4B] & 0xFFFFFF) << 5) == xfb, "XFB address", s->bp[0x4B], 0);
    check_eq("copy size", s->bp[0x4A], (479 << 10) | 639);
}

static void token_callback(u16 token)
{
    token_seen = token;
    token_calls++;
}

static void draw_done_callback(void)
{
    draw_done_calls++;
}

static void test_sync(void)
{
    int i;

    GXSetDrawSyncCallback(token_callback);
    GXSetDrawSync(0x1234);
    for (i = 0; i < 5000 && token_calls == 0; i++) {
        usleep(1000);
    }
    check_eq("token callbacks", token_calls, 1);
    check_eq("token", token_seen, 0x1234);
    check_eq("GXReadDrawSync", GXReadDrawSync(), 0x1234);
    GXSetDrawDoneCallback(draw_done_callback);
    for (i = 0; i < 100; i++) {
        GXDrawDone();
    }
    check_eq("draw done callbacks", draw_done_calls, 100);
    check(rec.finishes >= 101, "backend finishes", rec.finishes, 101);
}

// Random draws through the small ring; the GP's hash of every vertex must match the one written.
static void test_stream(void)
{
    u64 hash = 0xCBF29CE484222325ull;
    u32 seed = 12345, draws = 0, vertices = 0, i, v;
    GpStats st;

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT4, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT4, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    gp_fifo_sync();
    rec.hash = hash;
    rec.draws = 0;
    rec.vertices = 0;
    for (i = 0; i < 3000; i++) {
        u32 count;

        seed = seed * 1103515245 + 12345;
        // Every 500th draw is larger than the ring.
        count = i % 500 == 499 ? 6000 : 4 * (1 + (seed >> 16) % 100);
        GXBegin(GX_QUADS, GX_VTXFMT4, (u16) count);
        for (v = 0; v < count; v++) {
            u8 bytes[16];
            f32 x = (f32) (i * 1000 + v), y = (f32) seed, z = -0.5f;
            u32 c = seed ^ v;

            GXPosition3f32(x, y, z);
            GXColor1u32(c);
            memcpy(bytes, &x, 4);
            memcpy(bytes + 4, &y, 4);
            memcpy(bytes + 8, &z, 4);
            memcpy(bytes + 12, &c, 4);
            hash = fnv(hash, bytes, 16);
        }
        GXEnd();
        draws++;
        vertices += count;
    }
    gp_fifo_sync();
    gp_get_stats(&st);
    check_eq("stream draws", rec.draws, draws);
    check_eq("stream vertices", rec.vertices, vertices);
    check(rec.hash == hash, "stream hash", (u32) rec.hash, (u32) hash);
    check_eq("unknown opcodes", st.unknown_opcodes, 0);
    check_eq("bad addresses", st.bad_addresses, 0);
}

int main(void)
{
    pthread_t dog;
    void* fifo;
    GpStats st;

    pthread_create(&dog, NULL, watchdog, NULL);
    ps3_intr_init();
    arena = memalign(32, ARENA_SIZE);
    if (arena == NULL || !ps3_mem_add_region(arena, ARENA_SIZE)) {
        printf("FAIL: arena\n");
        return 1;
    }
    gp_init(&rec_backend, RING_SIZE);
    fifo = arena_alloc(0x40000);
    GXInit(fifo, 0x40000);
    GXFlush();
    gp_fifo_sync();
    gp_get_stats(&st);
    check_eq("GXInit unknown opcodes", st.unknown_opcodes, 0);
    test_init_state();
    test_matrices_lights();
    test_direct_draw();
    test_indexed_draw();
    test_display_list();
    test_tev_registers();
    test_depth_mode();
    test_textures_copies();
    test_sync();
    test_stream();
    gp_get_stats(&st);
    printf("GP: %u commands, %u draws, %u vertices, %u display lists\n", st.commands, st.draws, st.vertices,
           st.display_lists);
    printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
