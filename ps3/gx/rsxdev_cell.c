// The device layer on the RSX through PSL1GHT. Init, display and flip as DolphinPS3's RSXDevice and RSXPresenter,
// which run on hardware.
#include "rsxdev.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rsx/nv40.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

#include "../os/diag.h"
#include "../os/prof.h"
#include "../os/udplog.h"

// Main memory mapped for the RSX: the command buffer, then the vertex ring. The samples' 32 MB wastes RAM.
#define HOST_SIZE (8u << 20)
#define CB_SIZE (1u << 20)
#define RING_SIZE (HOST_SIZE - CB_SIZE)
#define RING_SEGMENTS 4
// Labels below 64 belong to the system.
#define LABEL_SYNC 200
#define LABEL_RING 201
#define FB_COUNT 3
#define WAIT_US 2000000
// Unverified on hardware: the GCM front face whose triangles are clockwise on the screen (window y down).
#define SCREEN_CW_FRONTFACE GCM_FRONTFACE_CW

static gcmContextData* ctx;
static u8* host;
static u8* ring;
static u32 ring_off, ring_pos, ring_seg;
static u32 ring_fence[RING_SEGMENTS], ring_value;
static u32 sync_value;
static u32 target_w, target_h;
static u32 last_depth_offset, last_depth_pitch;
static bool cull_cw_on, cull_ccw_on;

static u32 disp_w, disp_h, disp_pitch, disp_aspect;
static u32 disp_off[FB_COUNT], disp_depth_off;
static void* disp_mem[FB_COUNT];
static volatile u32 flips_done;
static u32 flips_queued, disp_next;
static u32 present_vp[8][4];
static u32 present_vp_n, present_vp_in, present_vp_out;
static u32 present_fp_off, present_fp_regs;
static u16 present_fp_tc;

static void (*vblank_fn)(void);
static void (*flush_hook)(void);

// librsx's call of the context callback (rsx_function_macros.h): it loads a 32-bit descriptor and switches r2.
s32 rsxContextCallback(gcmContextData* context, u32 count);

// The system's callback, and ours as the 32-bit descriptor (entry, TOC) rsxContextCallback calls.
static gcmContextCallback sys_callback;
static u32 hook_opd[2];

static void flush(void)
{
    if (flush_hook != NULL) {
        flush_hook();
    }
    rsxFlushBuffer(ctx);
}

// At each command buffer segment's end the system's callback moves PUT: the flush hook runs before it.
static s32 segment_callback(gcmContextData* context, u32 count)
{
    s32 r;

    if (flush_hook != NULL) {
        flush_hook();
    }
    context->callback = sys_callback;
    r = rsxContextCallback(context, count);
    context->callback = (gcmContextCallback) (uintptr_t) hook_opd;
    return r;
}

static void hook_segments(void)
{
    // An ELFv1 function pointer is its descriptor: entry, then TOC, both below 4 GB.
    const u64* opd = (const u64*) (uintptr_t) segment_callback;

    hook_opd[0] = (u32) opd[0];
    hook_opd[1] = (u32) opd[1];
    sys_callback = ctx->callback;
    ctx->callback = (gcmContextCallback) (uintptr_t) hook_opd;
    ps3_log("rsxdev: segment callback %08x wrapped (entry %08x, TOC %08x); segment %u bytes\n",
            (u32) (uintptr_t) sys_callback, hook_opd[0], hook_opd[1], (u32) (ctx->end - ctx->begin) * 4);
}

void dev_set_flush_hook(void (*fn)(void))
{
    flush_hook = fn;
}

static void on_flip(const u32 head)
{
    flips_done++;
}

void dev_debug_state(char* buf, unsigned size)
{
    gcmControlRegister* reg = gcmGetControlRegister();
    u32 get = reg->get, put = reg->put;
    int n = snprintf(buf, size, "flips %u queued, %u done; RSX get %08x put %08x; labels sync %u of %u, ring %u of %u",
                     flips_queued, flips_done, get, put, *(volatile u32*) gcmGetLabelAddress(LABEL_SYNC), sync_value,
                     *(volatile u32*) gcmGetLabelAddress(LABEL_RING), ring_value);

    // The command buffer is the start of the IO-mapped host region, so GET is an offset into host.
    if (get != put && get >= 16 && get + 16 <= CB_SIZE && n > 0 && (unsigned) n < size) {
        const volatile u32* w = (const volatile u32*) (host + get - 16);

        snprintf(buf + n, size - (unsigned) n, "; commands at get-16: %08x %08x %08x %08x | %08x %08x %08x %08x",
                 w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}

bool dev_debug_dump_stall(void)
{
    static u32 last_get = ~0u;
    static bool dumped;
    gcmControlRegister* reg = gcmGetControlRegister();
    u32 get = reg->get, put = reg->put;
    bool now = false;

    if (get != put && get == last_get && !dumped && get >= 512 && get <= CB_SIZE) {
        const volatile u32* w = (const volatile u32*) (host + get - 512);
        int i;

        dumped = true;
        now = true;
        for (i = 0; i < 128; i += 16) {
            ps3_log("rsxdev: stall at %08x, commands at %08x: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
                    "%08x %08x %08x %08x %08x %08x\n",
                    get, get - 512 + 4 * i, w[i], w[i + 1], w[i + 2], w[i + 3], w[i + 4], w[i + 5], w[i + 6],
                    w[i + 7], w[i + 8], w[i + 9], w[i + 10], w[i + 11], w[i + 12], w[i + 13], w[i + 14], w[i + 15]);
        }
    }
    last_get = get;
    return now;
}

static void on_vblank(const u32 head)
{
    if (vblank_fn != NULL) {
        vblank_fn();
    }
}

void dev_set_vblank(void (*fn)(void))
{
    vblank_fn = fn;
    gcmSetVBlankHandler(on_vblank);
}

static bool wait_label(u8 index, u32 value)
{
    volatile u32* label = (volatile u32*) gcmGetLabelAddress(index);
    u32 waited = 0;
    uint64_t t = ps3_prof_now();

    ps3_diag.gp_wait = 3;
    while ((s32) (*label - value) < 0) {
        if (waited > WAIT_US) {
            ps3_diag.gp_wait = 0;
            ps3_log("rsxdev: label %u not at %u after 2 s (it is %u)\n", index, value, *label);
            return false;
        }
        usleep(30);
        waited += 30;
    }
    ps3_diag.gp_wait = 0;
    ps3_prof_lap(PROF_GP_RSX, &t);
    return true;
}

void* dev_local_alloc(u32 align, u32 size, u32* offset)
{
    void* p = rsxMemalign(align < 64 ? 64 : align, size);

    if (p == NULL) {
        ps3_log("rsxdev: no local memory for %u bytes\n", size);
        return NULL;
    }
    rsxAddressToOffset(p, offset);
    return p;
}

void dev_local_free(void* ptr)
{
    if (ptr != NULL) {
        rsxFree(ptr);
    }
}

void dev_write_local(void* dst, const void* src, u32 size)
{
    volatile u32* d = dst;
    const u32* s = src;
    u32 i;

    for (i = 0; i < size / 4; i++) {
        d[i] = s[i];
    }
}

void* dev_ring_reserve(u32 min, u32* avail, u32* io_offset)
{
    const u32 seg = RING_SIZE / RING_SEGMENTS;
    // Vertex streams start 16-byte aligned: a misaligned float attribute wedges the vertex fetch.
    u32 pos = (ring_pos + 15) & ~15u;

    if (min > seg) {
        return NULL;
    }
    if (pos + min > (ring_seg + 1) * seg) {
        u32 next = (ring_seg + 1) % RING_SEGMENTS;

        // A backend label: the RSX writes it once the draws that read this segment have left the pipeline.
        rsxSetWriteBackendLabel(ctx, LABEL_RING, ++ring_value);
        flush();
        ring_fence[ring_seg] = ring_value;
        if (ring_fence[next] != 0) {
            wait_label(LABEL_RING, ring_fence[next]);
        }
        ring_seg = next;
        pos = next * seg;
    }
    ring_pos = pos;
    *avail = (ring_seg + 1) * seg - pos;
    *io_offset = ring_off + pos;
    return ring + pos;
}

void dev_ring_commit(u32 size)
{
    ring_pos += size;
}

void* dev_ring_alloc(u32 size, u32* io_offset)
{
    u32 avail;
    void* p = dev_ring_reserve(size, &avail, io_offset);

    if (p != NULL) {
        dev_ring_commit(size);
    }
    return p;
}

static void set_surface(u32 format, u32 color_offset, u32 color_pitch, u32 depth_offset, u32 depth_pitch, u32 w,
                        u32 h)
{
    gcmSurface sf;
    int i;

    memset(&sf, 0, sizeof(sf));
    sf.colorFormat = (u8) format;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0] = color_offset;
    sf.colorPitch[0] = color_pitch;
    for (i = 1; i < 4; i++) {
        sf.colorLocation[i] = GCM_LOCATION_RSX;
        sf.colorPitch[i] = 64;
    }
    sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = depth_offset;
    sf.depthPitch = depth_pitch;
    sf.type = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = (u16) w;
    sf.height = (u16) h;
    rsxSetSurface(ctx, &sf);
    for (i = 0; i < 8; i++) {
        rsxSetViewportClip(ctx, (u8) i, (u16) w, (u16) h);
    }
    target_w = w;
    target_h = h;
}

void dev_target(const DevTarget* t)
{
    // A colour-only target still names a depth buffer: the last real one, which is at least as large.
    if (t->depth_pitch != 0) {
        last_depth_offset = t->depth_offset;
        last_depth_pitch = t->depth_pitch;
    }
    set_surface(GCM_SURFACE_A8R8G8B8, t->color_offset, t->color_pitch, last_depth_offset, last_depth_pitch, t->width,
                t->height);
    rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
    rsxSetZMinMaxControl(ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
    rsxSetUserClipPlaneControl(ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
                               GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
                               GCM_USER_CLIP_PLANE_DISABLE);
    rsxSetColorMaskMrt(ctx, 0);
}

void dev_viewport(const float scale[4], const float offset[4])
{
    rsxSetViewport(ctx, 0, 0, (u16) target_w, (u16) target_h, 0.0f, 1.0f, scale, offset);
}

void dev_scissor(u32 x, u32 y, u32 w, u32 h)
{
    rsxSetScissor(ctx, (u16) x, (u16) y, (u16) w, (u16) h);
}

void dev_depth(bool test, u32 func, bool write)
{
    rsxSetDepthTestEnable(ctx, test ? GCM_TRUE : GCM_FALSE);
    rsxSetDepthFunc(ctx, func);
    rsxSetDepthWriteEnable(ctx, write ? GCM_TRUE : GCM_FALSE);
}

void dev_depth_bias(float units)
{
    if (units == 0.0f) {
        rsxSetPolygonOffsetFillEnable(ctx, GCM_FALSE);
        return;
    }
    rsxSetPolygonOffset(ctx, 0.0f, units);
    rsxSetPolygonOffsetFillEnable(ctx, GCM_TRUE);
}

void dev_blend(bool enable, u16 src, u16 dst, u16 eq)
{
    rsxSetBlendEnable(ctx, enable ? GCM_TRUE : GCM_FALSE);
    if (enable) {
        rsxSetBlendFunc(ctx, src, dst, src, dst);
        rsxSetBlendEquation(ctx, eq, eq);
    }
}

void dev_alpha_test(bool enable, u32 func, u8 ref)
{
    rsxSetAlphaTestEnable(ctx, enable ? GCM_TRUE : GCM_FALSE);
    if (enable) {
        rsxSetAlphaFunc(ctx, func, ref);
    }
}

void dev_color_mask(bool rgb, bool alpha)
{
    u32 mask = (rgb ? GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B : 0) | (alpha ? GCM_COLOR_MASK_A : 0);

    rsxSetColorMask(ctx, mask);
}

void dev_cull(bool cw, bool ccw)
{
    if (cw == cull_cw_on && ccw == cull_ccw_on) {
        return;
    }
    cull_cw_on = cw;
    cull_ccw_on = ccw;
    if (!cw && !ccw) {
        rsxSetCullFaceEnable(ctx, GCM_FALSE);
        return;
    }
    rsxSetFrontFace(ctx, SCREEN_CW_FRONTFACE);
    rsxSetCullFace(ctx, cw && ccw ? GCM_CULL_ALL : cw ? GCM_CULL_FRONT : GCM_CULL_BACK);
    rsxSetCullFaceEnable(ctx, GCM_TRUE);
}

void dev_point_size(float size)
{
    rsxSetPointSize(ctx, size < 1.0f ? 1.0f : size);
}

void dev_line_width(float width)
{
    // PSL1GHT has no line width command: lines stay one pixel wide.
    (void) width;
}

void dev_vertex_program(const u32 (*ucode)[4], u32 n, u32 start, u32 input_mask, u32 output_mask, u32 regs)
{
    rsxVertexProgram vp;

    memset(&vp, 0, sizeof(vp));
    vp.num_regs = (u16) regs;
    // With no instructions PSL1GHT only sets the load and start slots, the attribute masks and the register count.
    vp.num_insn = (u16) (ucode != NULL ? n : 0);
    vp.insn_start = (u16) start;
    vp.input_mask = input_mask;
    vp.output_mask = output_mask;
    rsxLoadVertexProgramBlock(ctx, &vp, ucode);
}

void dev_vp_constants(u32 first, u32 count, const float* v)
{
    rsxLoadVertexProgramParameterBlock(ctx, first, count, v);
}

void dev_fragment_program(u32 offset, u32 regs, u16 texcoords, u16 texcoords4)
{
    rsxFragmentProgram fp;

    memset(&fp, 0, sizeof(fp));
    fp.num_regs = (u16) regs;
    // The colour comes from R0, 32-bit.
    fp.fp_control = 0x40;
    fp.texcoords = texcoords;
    fp.texcoord2D = (u16) (texcoords & ~texcoords4);
    fp.texcoord3D = texcoords4;
    rsxLoadFragmentProgramLocation(ctx, &fp, offset, GCM_LOCATION_RSX);
}

void dev_texture(int unit, const DevTexture* t)
{
    gcmTexture g;

    rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
    if (t == NULL) {
        rsxTextureControl(ctx, (u8) unit, GCM_FALSE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
        return;
    }
    memset(&g, 0, sizeof(g));
    g.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | (t->swizzled ? GCM_TEXTURE_FORMAT_SWZ : GCM_TEXTURE_FORMAT_LIN);
    // A level count, not a flag.
    g.mipmap = t->levels ? t->levels : 1;
    g.dimension = GCM_TEXTURE_DIMS_2D;
    g.cubemap = GCM_FALSE;
    g.remap = (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
              (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
              (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
              (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
              (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
              (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
              (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
              (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
    g.width = t->width;
    g.height = t->height;
    g.depth = 1;
    g.location = GCM_LOCATION_RSX;
    g.pitch = t->swizzled ? 0 : t->pitch;
    g.offset = t->offset;
    rsxLoadTexture(ctx, (u8) unit, &g);
    // LODs in 4.8 fixed point. One level keeps DolphinPS3's 0 to 12, proven on hardware: max_lod 0 can show nothing.
    if (t->levels <= 1) {
        rsxTextureControl(ctx, (u8) unit, GCM_TRUE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    } else {
        rsxTextureControl(ctx, (u8) unit, GCM_TRUE, t->min_lod, t->max_lod, GCM_TEXTURE_MAX_ANISO_1);
    }
    rsxTextureFilter(ctx, (u8) unit, (u16) (t->lod_bias & 0x1FFF), t->min_filter, t->mag_filter,
                     GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(ctx, (u8) unit, t->wrap_s, t->wrap_t, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

// The attribute registers as dev_attribs last wrote them; attr_valid false after anything else wrote them.
static bool attr_valid;
static u32 attr_format[16], attr_offset[16];

void dev_attrib(int slot, u32 offset, u32 stride, u32 count, u32 type)
{
    attr_valid = false;
    rsxBindVertexArrayAttrib(ctx, (u8) slot, 0, offset & ~DEV_LOCAL, (u8) stride, (u8) count, (u8) type,
                             (offset & DEV_LOCAL) ? GCM_LOCATION_RSX : GCM_LOCATION_CELL);
}

void dev_attribs(const u32 format[16], const u32 offset[16], u32 base, u32 enabled)
{
    u32 f0, f1, o0, o1, n, i;
    u32* p;

    dev_attribs_diff(format, offset, base, enabled, attr_valid, attr_format, attr_offset, &f0, &f1, &o0, &o1);
    n = (f1 > f0 ? f1 - f0 + 1 : 0) + (o1 > o0 ? o1 - o0 + 1 : 0);
    if (n == 0) {
        return;
    }
    // As librsx: one reservation, ctx->current stored once. Read from the arguments, not the shadow just stored.
    if (ctx->current + n > ctx->end && rsxContextCallback(ctx, n) != 0) {
        attr_valid = false;
        return;
    }
    p = ctx->current;
    if (f1 > f0) {
        *p++ = (f1 - f0) << 18 | NV40TCL_VTXFMT(f0);
        for (i = f0; i < f1; i++) {
            *p++ = format[i];
        }
    }
    if (o1 > o0) {
        *p++ = (o1 - o0) << 18 | NV40TCL_VTXBUF_ADDRESS(o0);
        for (i = o0; i < o1; i++) {
            *p++ = dev_attribs_offset(offset, base, enabled, i);
        }
    }
    ctx->current = p;
    attr_valid = true;
}

void dev_draw(u32 prim, u32 count)
{
    // The ring is reused: the vertex cache must not keep old data at these offsets.
    rsxInvalidateVertexCache(ctx);
    rsxDrawVertexArray(ctx, prim, 0, count);
}

void dev_draw_indexed(u32 prim, u32 offset, u32 count)
{
    // rsxDrawIndexArray writes 0x1714 once; the full invalidate (0x1710, then 0x1714 three times) as dev_draw.
    rsxInvalidateVertexCache(ctx);
    rsxDrawIndexArray(ctx, (u8) prim, offset & ~DEV_LOCAL, count, GCM_INDEX_TYPE_16B,
                      (offset & DEV_LOCAL) ? GCM_LOCATION_RSX : GCM_LOCATION_CELL);
}

void dev_clear(bool color, bool alpha, bool depth, u32 argb, u32 z24)
{
    u32 mask = (color ? GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B : 0) | (alpha ? GCM_CLEAR_A : 0) |
               (depth ? GCM_CLEAR_Z | GCM_CLEAR_S : 0);

    if (mask == 0) {
        return;
    }
    rsxSetClearColor(ctx, argb);
    rsxSetClearDepthStencil(ctx, z24 << 8);
    rsxClearSurface(ctx, mask);
}

void dev_barrier(void)
{
    rsxSetWriteBackendLabel(ctx, LABEL_SYNC, ++sync_value);
    rsxSetWaitLabel(ctx, LABEL_SYNC, sync_value);
}

void dev_flush(void)
{
    flush();
}

void dev_finish(void)
{
    rsxSetWriteBackendLabel(ctx, LABEL_SYNC, ++sync_value);
    flush();
    wait_label(LABEL_SYNC, sync_value);
}

static bool init_display(void)
{
    videoState state;
    videoResolution res;
    videoConfiguration vc;
    int i;

    // The XMB's current mode: forcing another one can leave the TV without a picture.
    if (videoGetState(0, 0, &state) != 0 || videoGetResolution(state.displayMode.resolution, &res) != 0) {
        ps3_log("rsxdev: videoGetState failed\n");
        return false;
    }
    memset(&vc, 0, sizeof(vc));
    vc.resolution = state.displayMode.resolution;
    vc.format = VIDEO_BUFFER_FORMAT_XRGB;
    vc.aspect = state.displayMode.aspect;
    vc.pitch = (u32) res.width * 4;
    if (videoConfigure(0, &vc, NULL, 0) != 0) {
        ps3_log("rsxdev: videoConfigure failed (resolution %u)\n", (u32) vc.resolution);
        return false;
    }
    disp_w = res.width;
    disp_h = res.height;
    disp_pitch = disp_w * 4;
    disp_aspect = state.displayMode.aspect;
    ps3_log("rsxdev: video %ux%u, aspect %u\n", disp_w, disp_h, disp_aspect);
    gcmSetFlipMode(GCM_FLIP_VSYNC);
    gcmSetFlipHandler(on_flip);
    for (i = 0; i < FB_COUNT; i++) {
        disp_mem[i] = dev_local_alloc(64, disp_pitch * disp_h, &disp_off[i]);
        if (disp_mem[i] == NULL) {
            return false;
        }
        gcmSetDisplayBuffer((u8) i, disp_off[i], disp_pitch, disp_w, disp_h);
    }
    if (dev_local_alloc(64, disp_pitch * disp_h, &disp_depth_off) == NULL) {
        return false;
    }
    // Black on all buffers, buffer 0 shown.
    for (i = 0; i < FB_COUNT; i++) {
        set_surface(GCM_SURFACE_X8R8G8B8, disp_off[i], disp_pitch, disp_depth_off, disp_pitch, disp_w, disp_h);
        rsxSetScissor(ctx, 0, 0, (u16) disp_w, (u16) disp_h);
        rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
        rsxSetClearColor(ctx, 0xFF000000);
        rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
    }
    gcmSetFlip(ctx, 0);
    flush();
    gcmSetWaitFlip(ctx);
    flips_queued = 1;
    disp_next = 1;
    ps3_log("rsxdev: display buffers at %08x %08x %08x, first flip queued\n", disp_off[0], disp_off[1],
            disp_off[2]);
    dev_finish();
    ps3_log("rsxdev: init finished on the RSX\n");
    return true;
}

bool dev_init(void)
{
    host = memalign(1 << 20, HOST_SIZE);
    if (host == NULL || rsxInit(&ctx, CB_SIZE, HOST_SIZE, host) != 0 || ctx == NULL) {
        ps3_log("rsxdev: rsxInit failed\n");
        return false;
    }
    ring = host + CB_SIZE;
    rsxAddressToOffset(ring, &ring_off);
    hook_segments();
    ps3_log("rsxdev: rsxInit done, host %p, ring IO offset %08x\n", host, ring_off);
    return init_display();
}

void dev_set_present_programs(const u32 (*vp)[4], u32 vp_n, u32 vp_inputs, u32 vp_outputs, u32 fp_offset,
                              u32 fp_regs, u16 fp_texcoords)
{
    memcpy(present_vp, vp, 16u * (vp_n < 8 ? vp_n : 8));
    present_vp_n = vp_n < 8 ? vp_n : 8;
    present_vp_in = vp_inputs;
    present_vp_out = vp_outputs;
    present_fp_off = fp_offset;
    present_fp_regs = fp_regs;
    present_fp_tc = fp_texcoords;
}

// Draws the quad of pixels x0-x1, y0-y1 of the display with texture t, through the present programs. False when
// the ring has no room.
static bool present_quad(float x0, float x1, float y0, float y1, const DevTexture* t)
{
    typedef struct {
        float pos[4], tc[2];
    } Vertex;
    const float tu[4] = { 0, 1, 1, 0 }, tv[4] = { 0, 0, 1, 1 };
    float px[4], py[4];
    Vertex* v;
    u32 io;
    int i;

    v = dev_ring_alloc(4 * sizeof(Vertex), &io);
    if (v == NULL) {
        return false;
    }
    px[0] = px[3] = x0 / disp_w * 2.0f - 1.0f;
    px[1] = px[2] = x1 / disp_w * 2.0f - 1.0f;
    py[0] = py[1] = 1.0f - y0 / disp_h * 2.0f;
    py[2] = py[3] = 1.0f - y1 / disp_h * 2.0f;
    for (i = 0; i < 4; i++) {
        v[i].pos[0] = px[i];
        v[i].pos[1] = py[i];
        v[i].pos[2] = 0.5f;
        v[i].pos[3] = 1.0f;
        v[i].tc[0] = tu[i];
        v[i].tc[1] = tv[i];
    }
    dev_texture(0, t);
    dev_attrib(0, io, sizeof(Vertex), 4, GCM_VERTEX_DATA_TYPE_F32);
    dev_attrib(8, io + 16, sizeof(Vertex), 2, GCM_VERTEX_DATA_TYPE_F32);
    dev_draw(GCM_TYPE_QUADS, 4);
    return true;
}

// Two copies in local memory, used in turn: the RSX may still read the one the last present drew.
#define OVERLAY_BYTES (512u * 256u * 4u)
static void* overlay_mem[2];
static u32 overlay_off[2], overlay_serial;
static int overlay_cur;
static bool overlay_failed;

static void present_overlay(const DevOverlay* o)
{
    // A whole scale: 2 at 720p, 3 at 1080p.
    u32 scale = disp_h / 360 != 0 ? disp_h / 360 : 1, size, x0, y0;
    DevTexture t;
    int i;

    if (o == NULL || o->pixels == NULL || overlay_failed) {
        return;
    }
    size = (u32) o->w * o->h * 4;
    if (size > OVERLAY_BYTES || o->w % 16 != 0 || o->w * scale > disp_w || o->h * scale > disp_h) {
        return;
    }
    if (overlay_mem[0] == NULL) {
        for (i = 0; i < 2; i++) {
            overlay_mem[i] = dev_local_alloc(128, OVERLAY_BYTES, &overlay_off[i]);
            overlay_failed |= overlay_mem[i] == NULL;
        }
        if (overlay_failed) {
            return;
        }
    }
    if (o->serial != overlay_serial) {
        overlay_cur ^= 1;
        dev_write_local(overlay_mem[overlay_cur], o->pixels, size);
        overlay_serial = o->serial;
    }
    memset(&t, 0, sizeof(t));
    t.offset = overlay_off[overlay_cur];
    t.pitch = (u32) o->w * 4;
    t.width = o->w;
    t.height = o->h;
    t.levels = 1;
    t.wrap_s = t.wrap_t = DEV_WRAP_CLAMP;
    t.min_filter = t.mag_filter = DEV_FILTER_NEAREST;
    x0 = (disp_w - o->w * scale) / 2;
    y0 = (disp_h - o->h * scale) / 2;
    dev_blend(true, DEV_SRC_ALPHA, DEV_ONE_MINUS_SRC_ALPHA, DEV_EQ_ADD);
    present_quad((float) x0, (float) (x0 + o->w * scale), (float) y0, (float) (y0 + o->h * scale), &t);
    dev_blend(false, GCM_ONE, GCM_ZERO, GCM_FUNC_ADD);
}

void dev_present(u32 src_offset, u32 src_pitch, u32 w, u32 h, bool wide, const DevOverlay* overlay)
{
    float tv_aspect = disp_aspect == VIDEO_ASPECT_16_9 ? 16.0f / 9.0f : 4.0f / 3.0f;
    float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    // A picture wider than the TV keeps the full width and loses height.
    float iw = aspect <= tv_aspect ? (float) disp_w * aspect / tv_aspect : (float) disp_w;
    float ih = aspect <= tv_aspect ? (float) disp_h : (float) disp_h * tv_aspect / aspect;
    float left = (float) (u32) (((float) disp_w - iw) / 2.0f), top = (float) (u32) (((float) disp_h - ih) / 2.0f);
    float scale[4] = { disp_w * 0.5f, disp_h * -0.5f, 0.5f, 0.0f };
    float offset[4] = { disp_w * 0.5f, disp_h * 0.5f, 0.5f, 0.0f };
    DevTexture t;
    u32 b = disp_next;
    uint64_t t0;
    int i;

    // Three buffers: wait only while two flips are still pending.
    ps3_diag.gp_wait = 2;
    t0 = ps3_prof_now();
    while ((s32) (flips_queued - flips_done) > FB_COUNT - 2) {
        usleep(100);
    }
    ps3_prof_lap(PROF_GP_FLIP, &t0);
    ps3_diag.gp_wait = 0;
    set_surface(GCM_SURFACE_X8R8G8B8, disp_off[b], disp_pitch, disp_depth_off, disp_pitch, disp_w, disp_h);
    rsxSetViewport(ctx, 0, 0, (u16) disp_w, (u16) disp_h, 0.0f, 1.0f, scale, offset);
    rsxSetScissor(ctx, 0, 0, (u16) disp_w, (u16) disp_h);
    rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
    rsxSetClearColor(ctx, 0xFF000000);
    rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
    dev_depth(false, GCM_NEVER + 7, false);
    dev_blend(false, GCM_ONE, GCM_ZERO, GCM_FUNC_ADD);
    dev_alpha_test(false, 0, 0);
    dev_cull(false, false);
    memset(&t, 0, sizeof(t));
    t.offset = src_offset;
    t.pitch = src_pitch;
    t.width = (u16) w;
    t.height = (u16) h;
    t.levels = 1;
    t.wrap_s = t.wrap_t = DEV_WRAP_CLAMP;
    t.min_filter = t.mag_filter = DEV_FILTER_LINEAR;
    // PSL1GHT drops draws after a flip unless the program, attributes and textures are bound again.
    for (i = 0; i < 16; i++) {
        dev_attrib(i, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32);
    }
    dev_vertex_program((const u32(*)[4]) present_vp, present_vp_n, DEV_VP_SLOTS, present_vp_in, present_vp_out,
                       1);
    dev_fragment_program(present_fp_off, present_fp_regs, present_fp_tc, 0);
    if (!present_quad(left, (float) disp_w - left, top, (float) disp_h - top, &t)) {
        return;
    }
    present_overlay(overlay);
    // No gcmSetWaitFlip: it would hold the GX draws until the vblank. A buffer with a flip pending is not drawn.
    gcmSetFlip(ctx, (u8) b);
    // The flip runs gcm's state reset.
    attr_valid = false;
    flush();
    flips_queued++;
    if (flips_queued <= 4) {
        ps3_log("rsxdev: present %u queued (buffer %u, source %08x %ux%u), %u flips done\n", flips_queued, b,
                src_offset, w, h, flips_done);
    }
#ifdef PS3_DUMP_FRAMES
    if (flips_queued % PS3_DUMP_FRAMES == 0 && flips_queued <= 6 * PS3_DUMP_FRAMES) {
        dev_debug_dump("display", flips_queued, disp_mem[b], disp_pitch, disp_w, disp_h);
    }
#endif
    disp_next = (b + 1) % FB_COUNT;
}

#ifdef PS3_DUMP_FRAMES
void dev_debug_dump(const char* name, u32 n, const void* mem, u32 pitch, u32 w, u32 h)
{
    char path[128];
    FILE* f;
    u32 y;

    dev_finish();
    snprintf(path, sizeof(path), "/dev_hdd0/tmp/melee_%s_%u_%ux%u.raw", name, n, w, h);
    f = fopen(path, "wb");
    if (f == NULL) {
        ps3_log("rsxdev: cannot write %s\n", path);
        return;
    }
    for (y = 0; y < h; y++) {
        fwrite((const u8*) mem + y * pitch, 4, w, f);
    }
    fclose(f);
    ps3_log("rsxdev: wrote %s\n", path);
}
#endif
