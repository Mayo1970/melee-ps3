// The device layer in software for the host tests: draws run at once on the ucode interpreters. Big-endian hosts
// only (qemu-ppc64): vertex and texel data are read as the RSX reads them.
#include "../gx/rsxdev.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rsx_interp.h"

#define LOCAL_SIZE (96u << 20)
#define RING_SIZE (16u << 20)
// As on the PS3, a reservation ends at a segment end.
#define RING_SEGMENT (RING_SIZE / 4)
#define SCREEN_W 640
#define SCREEN_H 480
#define MAX_BLOCKS 8192

static u8* local;
// The live allocations by offset; the game's caches free and allocate all the time.
static struct {
    u32 offset, size;
} blocks[MAX_BLOCKS];
static u32 nblocks;
static u8* ring;
static u32 ring_pos;

static DevTarget target;
static float vp_scale[4], vp_offset[4];
static u32 sc_x, sc_y, sc_w, sc_h;
static bool depth_test, depth_write;
static u32 depth_func;
static float depth_bias;
static bool blend_on;
static u16 blend_src, blend_dst, blend_eq;
static bool alpha_on;
static u32 alpha_func;
static u8 alpha_ref;
static bool mask_rgb = true, mask_alpha = true;
static bool cull_cw, cull_ccw;
static float point_size = 1.0f;
static u32 vp_code[512][4];
static u32 vp_start, vp_n;
static float vconst[468][4];
static u32 fp_offset;
static DevTexture tex[16];
static bool tex_on[16];
static struct {
    u32 offset, stride, count, type;
} attr[16];

static u32 screen[SCREEN_W * SCREEN_H];
static u32 presents;
static char dump_prefix[256];
static u32 dump_every = 1;
// No frame before this present is drawn or written.
u32 soft_dump_from;
static void (*vblank_fn)(void);
// As rsxdev_cell.c's PUT moves: before a segment switch, a flush or finish, and each draw this device draws.
static void (*flush_hook)(void);

void dev_set_flush_hook(void (*fn)(void))
{
    flush_hook = fn;
}

static void run_flush_hook(void)
{
    if (flush_hook != NULL) {
        flush_hook();
    }
}

// Draws drawn and pixels written, for the tests.
u32 soft_draws, soft_pixels;
// Draws and clears are counted, not drawn: the host game build runs the backend at speed.
bool soft_drop;
// With soft_drop: the 3 frames before a dump are drawn (the XFB shown is an earlier frame's copy).
bool soft_sparse;

static bool dropping(void)
{
    return soft_drop &&
           !(soft_sparse && dump_prefix[0] != 0 && presents >= soft_dump_from && dump_every - presents % dump_every <= 3);
}

void soft_set_dump(const char* prefix)
{
    snprintf(dump_prefix, sizeof(dump_prefix), "%s", prefix);
}

// Only every nth present is written.
void soft_set_dump_every(u32 n)
{
    dump_every = n ? n : 1;
}

const u32* soft_screen(void)
{
    return screen;
}

// A pixel of the current target, for the tests.
u32 soft_target_pixel(u32 x, u32 y)
{
    return *(u32*) (local + target.color_offset + y * target.color_pitch + x * 4);
}

bool dev_init(void)
{
    local = calloc(1, LOCAL_SIZE);
    ring = calloc(1, RING_SIZE);
    return local != NULL && ring != NULL;
}

// First fit between the live blocks; offset 0 stays unused, as on the RSX.
void* dev_local_alloc(u32 align, u32 size, u32* offset)
{
    u32 prev_end = 0x1000, i;

    if (nblocks == MAX_BLOCKS) {
        return NULL;
    }
    for (i = 0; i <= nblocks; i++) {
        u32 off = (prev_end + align - 1) & ~(align - 1);
        u32 limit = i < nblocks ? blocks[i].offset : LOCAL_SIZE;

        if (off <= limit && size <= limit - off) {
            memmove(&blocks[i + 1], &blocks[i], sizeof(blocks[0]) * (nblocks - i));
            blocks[i].offset = off;
            blocks[i].size = size;
            nblocks++;
            *offset = off;
            return local + off;
        }
        if (i < nblocks) {
            prev_end = blocks[i].offset + blocks[i].size;
        }
    }
    return NULL;
}

void dev_local_free(void* ptr)
{
    u32 off = (u32) ((u8*) ptr - local), i;

    if (ptr == NULL) {
        return;
    }
    for (i = 0; i < nblocks; i++) {
        if (blocks[i].offset == off) {
            memmove(&blocks[i], &blocks[i + 1], sizeof(blocks[0]) * (nblocks - i - 1));
            nblocks--;
            return;
        }
    }
    fprintf(stderr, "rsxdev_soft: free of %p, which is no allocation\n", ptr);
    abort();
}

void dev_write_local(void* dst, const void* src, u32 size)
{
    memcpy(dst, src, size);
}

void* dev_ring_reserve(u32 min, u32* avail, u32* io_offset)
{
    u32 pos = (ring_pos + 15) & ~15u, end;

    if (min > RING_SEGMENT) {
        return NULL;
    }
    pos = pos >= RING_SIZE ? 0 : pos;
    end = (pos / RING_SEGMENT + 1) * RING_SEGMENT;
    if (pos + min > end) {
        run_flush_hook();
        pos = end % RING_SIZE;
        end = pos + RING_SEGMENT;
    }
    ring_pos = pos;
    *avail = end - pos;
    *io_offset = pos;
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

void dev_target(const DevTarget* t)
{
    target = *t;
}

void dev_viewport(const float scale[4], const float offset[4])
{
    memcpy(vp_scale, scale, 16);
    memcpy(vp_offset, offset, 16);
}

void dev_scissor(u32 x, u32 y, u32 w, u32 h)
{
    sc_x = x;
    sc_y = y;
    sc_w = w;
    sc_h = h;
}

void dev_depth(bool test, u32 func, bool write)
{
    depth_test = test;
    depth_func = func & 7;
    depth_write = write;
}

void dev_depth_bias(float units)
{
    depth_bias = units;
}

void dev_blend(bool enable, u16 src, u16 dst, u16 eq)
{
    blend_on = enable;
    blend_src = src;
    blend_dst = dst;
    blend_eq = eq;
}

void dev_alpha_test(bool enable, u32 func, u8 ref)
{
    alpha_on = enable;
    alpha_func = func & 7;
    alpha_ref = ref;
}

void dev_color_mask(bool rgb, bool alpha)
{
    mask_rgb = rgb;
    mask_alpha = alpha;
}

void dev_cull(bool cw, bool ccw)
{
    cull_cw = cw;
    cull_ccw = ccw;
}

void dev_point_size(float size)
{
    point_size = size < 1.0f ? 1.0f : size;
}

void dev_line_width(float width)
{
    (void) width;
}

void dev_vertex_program(const u32 (*ucode)[4], u32 n, u32 start, u32 input_mask, u32 output_mask, u32 regs)
{
    if (start + n > 512) {
        fprintf(stderr, "rsxdev_soft: program of %u instructions at slot %u\n", n, start);
        abort();
    }
    if (ucode != NULL) {
        memcpy(vp_code[start], ucode, 16u * n);
    }
    vp_start = start;
    vp_n = n;
}

void dev_vp_constants(u32 first, u32 count, const float* v)
{
    memcpy(vconst[first], v, 16u * count);
}

void dev_fragment_program(u32 offset, u32 regs, u16 texcoords, u16 texcoords4)
{
    fp_offset = offset;
}

void dev_texture(int unit, const DevTexture* t)
{
    tex_on[unit] = t != NULL;
    if (t != NULL) {
        tex[unit] = *t;
    }
}

void dev_attrib(int slot, u32 offset, u32 stride, u32 count, u32 type)
{
    attr[slot].offset = offset;
    attr[slot].stride = stride;
    attr[slot].count = count;
    attr[slot].type = type;
}

static bool compare(u32 func, float a, float b)
{
    switch (func) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    default: return true;
    }
}

static float wrap(float c, u8 mode)
{
    if (mode == DEV_WRAP_REPEAT) {
        return c - floorf(c);
    }
    if (mode == DEV_WRAP_MIRROR) {
        float f = c - 2.0f * floorf(c / 2.0f);
        return f > 1.0f ? 2.0f - f : f;
    }
    return c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
}

static u32 log2u(u32 v)
{
    u32 n = 0;

    while ((1u << n) < v) {
        n++;
    }
    return n;
}

static void texel(const DevTexture* t, int x, int y, float out[4])
{
    u32 v, off;

    x = x < 0 ? 0 : x >= t->width ? t->width - 1 : x;
    y = y < 0 ? 0 : y >= t->height ? t->height - 1 : y;
    if (t->swizzled) {
        u32 lw = log2u(t->width), lh = log2u(t->height), s = 0, bit = 0, i;

        for (i = 0; i < (lw > lh ? lw : lh); i++) {
            if (i < lw) {
                s |= (((u32) x >> i) & 1) << bit++;
            }
            if (i < lh) {
                s |= (((u32) y >> i) & 1) << bit++;
            }
        }
        off = t->offset + s * 4;
    } else {
        off = t->offset + (u32) y * t->pitch + (u32) x * 4;
    }
    memcpy(&v, local + off, 4);
    out[0] = (float) ((v >> 16) & 0xFF) / 255.0f;
    out[1] = (float) ((v >> 8) & 0xFF) / 255.0f;
    out[2] = (float) (v & 0xFF) / 255.0f;
    out[3] = (float) (v >> 24) / 255.0f;
}

static void sample(void* user, int unit, const float coord[4], float out[4])
{
    const DevTexture* t = &tex[unit];
    float u, v;

    if (!tex_on[unit]) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    u = wrap(coord[0], t->wrap_s) * t->width;
    v = wrap(coord[1], t->wrap_t) * t->height;
    if (t->mag_filter == DEV_FILTER_LINEAR) {
        float a[4], b[4], c[4], d[4], fx, fy;
        int x0 = (int) floorf(u - 0.5f), y0 = (int) floorf(v - 0.5f), i;

        fx = u - 0.5f - (float) x0;
        fy = v - 0.5f - (float) y0;
        texel(t, x0, y0, a);
        texel(t, x0 + 1, y0, b);
        texel(t, x0, y0 + 1, c);
        texel(t, x0 + 1, y0 + 1, d);
        for (i = 0; i < 4; i++) {
            out[i] = (a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (c[i] * (1 - fx) + d[i] * fx) * fy;
        }
    } else {
        texel(t, (int) floorf(u), (int) floorf(v), out);
    }
}

static float blend_factor(u16 f, const float* src, const float* dst, int ch)
{
    switch (f) {
    case DEV_ZERO: return 0.0f;
    case DEV_ONE: return 1.0f;
    case DEV_SRC_COLOR: return src[ch];
    case DEV_ONE_MINUS_SRC_COLOR: return 1.0f - src[ch];
    case DEV_SRC_ALPHA: return src[3];
    case DEV_ONE_MINUS_SRC_ALPHA: return 1.0f - src[3];
    case DEV_DST_ALPHA: return dst[3];
    case DEV_ONE_MINUS_DST_ALPHA: return 1.0f - dst[3];
    case DEV_DST_COLOR: return dst[ch];
    default: return 1.0f - dst[ch];
    }
}

static float clamp01(float x)
{
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

typedef struct Vert {
    float out[16][4]; // the vertex program's outputs
    float x, y, z, iw; // window position, 1/w
} Vert;

static void shade(const Vert* v0, const Vert* v1, const Vert* v2, float b0, float b1, float b2, int px, int py)
{
    float in[16][4], col[4], dst[4], z, iw;
    u32* cp = (u32*) (local + target.color_offset + (u32) py * target.color_pitch + (u32) px * 4);
    u32* zp = target.depth_pitch ? (u32*) (local + target.depth_offset + (u32) py * target.depth_pitch + (u32) px * 4)
                                 : NULL;
    int i, k;

    z = clamp01(b0 * v0->z + b1 * v1->z + b2 * v2->z + depth_bias / 16777215.0f);
    if (zp != NULL && depth_test && !compare(depth_func, z, (float) (*zp >> 8) / 16777215.0f)) {
        return;
    }
    iw = b0 * v0->iw + b1 * v1->iw + b2 * v2->iw;
    memset(in, 0, sizeof(in));
    for (k = 0; k < 16; k++) {
        int o = k == 1 ? 1 : k == 2 ? 2 : k >= 4 && k < 12 ? 7 + (k - 4) : -1;

        if (o < 0) {
            continue;
        }
        for (i = 0; i < 4; i++) {
            in[k][i] = (b0 * v0->out[o][i] * v0->iw + b1 * v1->out[o][i] * v1->iw + b2 * v2->out[o][i] * v2->iw) / iw;
        }
        if (k == 1 || k == 2) {
            for (i = 0; i < 4; i++) {
                in[k][i] = clamp01(in[k][i]);
            }
        }
    }
    in[0][0] = (float) px + 0.5f;
    in[0][1] = (float) py + 0.5f;
    in[0][2] = z;
    in[0][3] = iw;
    fp_run((const u32*) (local + fp_offset), 8192, (const float(*)[4]) in, sample, NULL, col);
    for (i = 0; i < 4; i++) {
        col[i] = clamp01(col[i]);
    }
    if (alpha_on && !compare(alpha_func, floorf(col[3] * 255.0f + 0.5f), (float) alpha_ref)) {
        return;
    }
    dst[0] = (float) ((*cp >> 16) & 0xFF) / 255.0f;
    dst[1] = (float) ((*cp >> 8) & 0xFF) / 255.0f;
    dst[2] = (float) (*cp & 0xFF) / 255.0f;
    dst[3] = (float) (*cp >> 24) / 255.0f;
    if (blend_on) {
        float r[4];

        for (i = 0; i < 4; i++) {
            float s = col[i] * blend_factor(blend_src, col, dst, i), d = dst[i] * blend_factor(blend_dst, col, dst, i);
            r[i] = clamp01(blend_eq == DEV_EQ_REVERSE_SUBTRACT ? d - s : s + d);
        }
        memcpy(col, r, 16);
    }
    if (!mask_rgb) {
        memcpy(col, dst, 12);
    }
    if (!mask_alpha) {
        col[3] = dst[3];
    }
    *cp = (u32) (col[3] * 255.0f + 0.5f) << 24 | (u32) (col[0] * 255.0f + 0.5f) << 16 |
          (u32) (col[1] * 255.0f + 0.5f) << 8 | (u32) (col[2] * 255.0f + 0.5f);
    if (zp != NULL && depth_test && depth_write) {
        *zp = (u32) (z * 16777215.0f) << 8 | (*zp & 0xFF);
    }
    soft_pixels++;
}

static void triangle(const Vert* a, const Vert* b, const Vert* c)
{
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    int x0, x1, y0, y1, x, y;

    if (a->iw <= 0.0f || b->iw <= 0.0f || c->iw <= 0.0f || area == 0.0f) {
        return;
    }
    // y points down: a positive area is clockwise on the screen.
    if ((area > 0.0f && cull_cw) || (area < 0.0f && cull_ccw)) {
        return;
    }
    x0 = (int) floorf(fminf(a->x, fminf(b->x, c->x)));
    x1 = (int) ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    y0 = (int) floorf(fminf(a->y, fminf(b->y, c->y)));
    y1 = (int) ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    x0 = x0 < (int) sc_x ? (int) sc_x : x0;
    y0 = y0 < (int) sc_y ? (int) sc_y : y0;
    x1 = x1 > (int) (sc_x + sc_w) ? (int) (sc_x + sc_w) : x1;
    y1 = y1 > (int) (sc_y + sc_h) ? (int) (sc_y + sc_h) : y1;
    x1 = x1 > target.width ? target.width : x1;
    y1 = y1 > target.height ? target.height : y1;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            float px = (float) x + 0.5f, py = (float) y + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (c->x - px) * (b->y - py)) / area;
            float w1 = ((c->x - px) * (a->y - py) - (a->x - px) * (c->y - py)) / area;
            float w2 = 1.0f - w0 - w1;

            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                shade(a, b, c, w0, w1, w2, x, y);
            }
        }
    }
}

static const u8* vertex_mem(u32 offset)
{
    return (offset & DEV_LOCAL) ? local + (offset & ~DEV_LOCAL) : ring + offset;
}

static void fetch(u32 index, float in[16][4])
{
    int s, k;

    for (s = 0; s < 16; s++) {
        const u8* p = vertex_mem(attr[s].offset) + index * attr[s].stride;

        in[s][0] = in[s][1] = in[s][2] = 0.0f;
        in[s][3] = 1.0f;
        for (k = 0; k < (int) attr[s].count; k++) {
            switch (attr[s].type) {
            case 2:
                memcpy(&in[s][k], p + 4 * k, 4);
                break;
            case 4:
                in[s][k] = (float) p[k] / 255.0f;
                break;
            case 5:
                in[s][k] = (float) (s16) (p[2 * k] << 8 | p[2 * k + 1]);
                break;
            default:
                in[s][k] = (float) p[k];
                break;
            }
        }
    }
}

static void transform(u32 index, Vert* v)
{
    float in[16][4];
    float w;

    fetch(index, in);
    memset(v->out, 0, sizeof(v->out));
    vp_run((const u32(*)[4]) vp_code[vp_start], (int) vp_n, (const float(*)[4]) vconst, (const float(*)[4]) in, v->out);
    w = v->out[0][3];
    v->iw = w != 0.0f ? 1.0f / w : 0.0f;
    v->x = v->out[0][0] * v->iw * vp_scale[0] + vp_offset[0];
    v->y = v->out[0][1] * v->iw * vp_scale[1] + vp_offset[1];
    v->z = v->out[0][2] * v->iw * vp_scale[2] + vp_offset[2];
}

// Points, and lines too: squares of the point size.
static void point(const Vert* v)
{
    float h = point_size * 0.5f;
    Vert q[4];
    int k;

    for (k = 0; k < 4; k++) {
        q[k] = *v;
        q[k].x += (k == 1 || k == 2) ? h : -h;
        q[k].y += (k >= 2) ? h : -h;
    }
    triangle(&q[0], &q[1], &q[2]);
    triangle(&q[0], &q[2], &q[3]);
}

void dev_draw(u32 prim, u32 count)
{
    Vert* v = malloc(sizeof(Vert) * (count ? count : 1));
    u32 i;

    soft_draws++;
    if (dropping()) {
        free(v);
        return;
    }
    run_flush_hook();
    for (i = 0; i < count; i++) {
        transform(i, &v[i]);
    }
    switch (prim) {
    case DEV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < count; i += 3) {
            triangle(&v[i], &v[i + 1], &v[i + 2]);
        }
        break;
    case DEV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < count; i++) {
            if (i & 1) {
                triangle(&v[i + 1], &v[i], &v[i + 2]);
            } else {
                triangle(&v[i], &v[i + 1], &v[i + 2]);
            }
        }
        break;
    case DEV_PRIM_TRIANGLE_FAN:
        for (i = 1; i + 1 < count; i++) {
            triangle(&v[0], &v[i], &v[i + 1]);
        }
        break;
    case DEV_PRIM_QUADS:
        for (i = 0; i + 3 < count; i += 4) {
            triangle(&v[i], &v[i + 1], &v[i + 2]);
            triangle(&v[i], &v[i + 2], &v[i + 3]);
        }
        break;
    default:
        for (i = 0; i < count; i++) {
            point(&v[i]);
        }
        break;
    }
    free(v);
}

void dev_draw_indexed(u32 prim, u32 offset, u32 count)
{
    const u8* p = vertex_mem(offset);
    u32 n = 0, i;
    Vert* v;

    soft_draws++;
    if (dropping()) {
        return;
    }
    run_flush_hook();
    for (i = 0; i < count; i++) {
        u32 k = (u32) (p[2 * i] << 8 | p[2 * i + 1]);

        n = k + 1 > n ? k + 1 : n;
    }
    v = malloc(sizeof(Vert) * (n ? n : 1));
    for (i = 0; i < n; i++) {
        transform(i, &v[i]);
    }
#define IDX(j) ((u32) (p[2 * (j)] << 8 | p[2 * (j) + 1]))
    if (prim == DEV_PRIM_TRIANGLES) {
        for (i = 0; i + 2 < count; i += 3) {
            triangle(&v[IDX(i)], &v[IDX(i + 1)], &v[IDX(i + 2)]);
        }
    } else {
        for (i = 0; i < count; i++) {
            point(&v[IDX(i)]);
        }
    }
#undef IDX
    free(v);
}

void dev_clear(bool color, bool alpha, bool depth, u32 argb, u32 z24)
{
    u32 x, y;

    if (dropping()) {
        return;
    }
    for (y = sc_y; y < sc_y + sc_h && y < target.height; y++) {
        for (x = sc_x; x < sc_x + sc_w && x < target.width; x++) {
            u32* cp = (u32*) (local + target.color_offset + y * target.color_pitch + x * 4);

            if (color) {
                *cp = (*cp & 0xFF000000) | (argb & 0xFFFFFF);
            }
            if (alpha) {
                *cp = (*cp & 0xFFFFFF) | (argb & 0xFF000000);
            }
            if (depth && target.depth_pitch) {
                *(u32*) (local + target.depth_offset + y * target.depth_pitch + x * 4) = z24 << 8;
            }
        }
    }
}

void dev_barrier(void) {}

void dev_flush(void)
{
    run_flush_hook();
}

void dev_finish(void)
{
    run_flush_hook();
}

void dev_set_present_programs(const u32 (*vp)[4], u32 vp_n, u32 vp_inputs, u32 vp_outputs, u32 fp_offset,
                              u32 fp_regs, u16 fp_texcoords)
{
}

void dev_present(u32 src_offset, u32 src_pitch, u32 w, u32 h)
{
    char path[300];
    FILE* f;
    u32 x, y;

    presents++;
    if (soft_drop && (dump_prefix[0] == 0 || presents % dump_every != 0 || presents < soft_dump_from)) {
        return;
    }
    for (y = 0; y < SCREEN_H; y++) {
        for (x = 0; x < SCREEN_W; x++) {
            u32 sx = x * w / SCREEN_W, sy = y * h / SCREEN_H;
            screen[y * SCREEN_W + x] = *(u32*) (local + src_offset + sy * src_pitch + sx * 4);
        }
    }
    if (dump_prefix[0] == 0 || presents % dump_every != 0) {
        return;
    }
    snprintf(path, sizeof(path), "%s%03u.ppm", dump_prefix, presents);
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (y = 0; y < SCREEN_W * SCREEN_H; y++) {
        u8 rgb[3] = { (u8) (screen[y] >> 16), (u8) (screen[y] >> 8), (u8) screen[y] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

// The vertical blank at 59.94 Hz on a host thread, from absolute times so it does not drift.
static void* vblank_main(void* arg)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    for (;;) {
        t.tv_nsec += 16683350;
        if (t.tv_nsec >= 1000000000) {
            t.tv_nsec -= 1000000000;
            t.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        vblank_fn();
    }
    return NULL;
}

void dev_set_vblank(void (*fn)(void))
{
    pthread_t thread;

    vblank_fn = fn;
    pthread_create(&thread, NULL, vblank_main, NULL);
    pthread_detach(thread);
}

void dev_debug_state(char* buf, unsigned size)
{
    snprintf(buf, size, "software RSX: %u draws, %u presents, %u local blocks", soft_draws, presents, nblocks);
}

bool dev_debug_dump_stall(void)
{
    return false;
}
