// The RSX as the GX backend uses it. rsxdev_cell.c drives the RSX through PSL1GHT; tests/rsxdev_soft.c draws the
// same calls in software. stdint types only: PSL1GHT's and the SDK's u64 differ, so their headers never meet.
#ifndef PS3_GX_RSXDEV_H
#define PS3_GX_RSXDEV_H

#include <stdbool.h>
#include <stdint.h>

// GCM values the backend passes through.
#define DEV_PRIM_POINTS 1
#define DEV_PRIM_LINES 2
#define DEV_PRIM_LINE_STRIP 4
#define DEV_PRIM_TRIANGLES 5
#define DEV_PRIM_TRIANGLE_STRIP 6
#define DEV_PRIM_TRIANGLE_FAN 7
#define DEV_PRIM_QUADS 8
#define DEV_FUNC_NEVER 0x0200 // + GX compare function 0-7
#define DEV_ZERO 0
#define DEV_ONE 1
#define DEV_SRC_COLOR 0x0300
#define DEV_ONE_MINUS_SRC_COLOR 0x0301
#define DEV_SRC_ALPHA 0x0302
#define DEV_ONE_MINUS_SRC_ALPHA 0x0303
#define DEV_DST_ALPHA 0x0304
#define DEV_ONE_MINUS_DST_ALPHA 0x0305
#define DEV_DST_COLOR 0x0306
#define DEV_ONE_MINUS_DST_COLOR 0x0307
#define DEV_EQ_ADD 0x8006
#define DEV_EQ_REVERSE_SUBTRACT 0x800B
#define DEV_WRAP_REPEAT 1
#define DEV_WRAP_MIRROR 2
#define DEV_WRAP_CLAMP 3
#define DEV_FILTER_NEAREST 1
#define DEV_FILTER_LINEAR 2
#define DEV_FILTER_NEAREST_MIP_NEAREST 3
#define DEV_FILTER_LINEAR_MIP_NEAREST 4
#define DEV_FILTER_NEAREST_MIP_LINEAR 5
#define DEV_FILTER_LINEAR_MIP_LINEAR 6
// Vertex program slots for the backend; the present pass loads its program into the 8 after them (512 in all).
#define DEV_VP_SLOTS 504
// A vertex or index offset with this bit is in local memory; without it, an IO offset (the vertex ring).
#define DEV_LOCAL 0x80000000u

typedef struct DevTarget {
    uint32_t color_offset, color_pitch;
    uint32_t depth_offset, depth_pitch; // depth_pitch 0: no depth buffer
    uint16_t width, height;
} DevTarget;

// An A8R8G8B8 texture in local memory: linear (pitch bytes per row, one level) or swizzled (power-of-two sides,
// the levels one after the other).
typedef struct DevTexture {
    uint32_t offset, pitch;
    uint16_t width, height;
    uint8_t levels, swizzled;
    uint8_t wrap_s, wrap_t, min_filter, mag_filter;
    uint16_t min_lod, max_lod; // 4.8 fixed point
    int16_t lod_bias;         // 4.8 fixed point, signed
} DevTexture;

bool dev_init(void);
void* dev_local_alloc(uint32_t align, uint32_t size, uint32_t* offset);
void dev_local_free(void* ptr);
// Sequential stores into local memory (write-combined on the PS3); size a multiple of 4.
void dev_write_local(void* dst, const void* src, uint32_t size);
// The vertex ring in main memory the RSX reads. Returns NULL when size does not fit; may wait for the RSX.
void* dev_ring_alloc(uint32_t size, uint32_t* io_offset);
// The free ring space up to the end of its segment, at least min bytes, 16-byte aligned; it may wait for the RSX.
// dev_ring_commit then keeps the first size bytes. No other ring call between the two.
void* dev_ring_reserve(uint32_t min, uint32_t* avail, uint32_t* io_offset);
void dev_ring_commit(uint32_t size);

void dev_target(const DevTarget* t);
void dev_viewport(const float scale[4], const float offset[4]);
void dev_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void dev_depth(bool test, uint32_t func, bool write);
// Moves later polygons by units of the smallest depth step (negative: nearer); 0 turns the offset off.
void dev_depth_bias(float units);
void dev_blend(bool enable, uint16_t src, uint16_t dst, uint16_t eq);
void dev_alpha_test(bool enable, uint32_t func, uint8_t ref);
void dev_color_mask(bool rgb, bool alpha);
// cull_cw: cull the primitives that are clockwise on the screen; cull_ccw the others.
void dev_cull(bool cull_cw, bool cull_ccw);
void dev_point_size(float size);
void dev_line_width(float width);
// Runs the n instructions at program slot start; ucode loads them there first, NULL means they are there.
// regs: temps used.
void dev_vertex_program(const uint32_t (*ucode)[4], uint32_t n, uint32_t start, uint32_t input_mask,
                        uint32_t output_mask, uint32_t regs);
void dev_vp_constants(uint32_t first, uint32_t count, const float* v);
// texcoords4: texcoords read with more than two components.
void dev_fragment_program(uint32_t offset, uint32_t regs, uint16_t texcoords, uint16_t texcoords4);
// NULL disables the unit.
void dev_texture(int unit, const DevTexture* t);
// An attribute at offset (IO, or local with DEV_LOCAL); count 0 disables it.
void dev_attrib(int slot, uint32_t offset, uint32_t stride, uint32_t count, uint32_t type);
void dev_draw(uint32_t prim, uint32_t count);
// count u16 indices at offset (IO, or local with DEV_LOCAL), into the vertices of the bound attributes.
void dev_draw_indexed(uint32_t prim, uint32_t offset, uint32_t count);
// Clears the scissor rectangle of the target.
void dev_clear(bool color, bool alpha, bool depth, uint32_t argb, uint32_t z24);
// Later draws wait until the RSX has finished all earlier work (render to texture, then sample it).
void dev_barrier(void);
// fn runs before each move of the RSX's PUT, the command buffer segments' too, after which the RSX reads what is
// queued (vx_drain). It must not send RSX commands.
void dev_set_flush_hook(void (*fn)(void));
void dev_flush(void);
// Waits until the RSX has run everything queued so far.
void dev_finish(void);
// The present pass's programs: position and texcoord 0 as they are, the texel as it is (the backend builds them).
void dev_set_present_programs(const uint32_t (*vp)[4], uint32_t vp_n, uint32_t vp_inputs, uint32_t vp_outputs,
                              uint32_t fp_offset, uint32_t fp_regs, uint16_t fp_texcoords);
// Shows the A8R8G8B8 rectangle at local src (w x h, pitch bytes) on the screen at 4:3, and flips.
void dev_present(uint32_t src_offset, uint32_t src_pitch, uint32_t w, uint32_t h);
// fn runs on each vertical blank of the display, on a system thread. Call after dev_init.
void dev_set_vblank(void (*fn)(void));
// The flips, the RSX's GET and PUT and the fence labels, as text (step 3 diagnostics).
void dev_debug_state(char* buf, unsigned size);
// Logs the 512 bytes of commands before GET once, when GET has not moved since the last call and differs from PUT.
// Returns true on that call.
bool dev_debug_dump_stall(void);
#ifdef PS3_DUMP_FRAMES
// RPCS3 builds only: waits for the RSX, then writes the w x h pixels at mem to /dev_hdd0/tmp as raw XRGB.
void dev_debug_dump(const char* name, uint32_t n, const void* mem, uint32_t pitch, uint32_t w, uint32_t h);
#endif

#endif
