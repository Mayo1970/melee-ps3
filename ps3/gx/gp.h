// The GameCube GP behind the SDK's GX library: the GP thread decodes the FIFO the library writes and keeps
// the CP, XF and BP registers; a backend draws from them. Register numbers as in Dolphin's VideoCommon.
#ifndef PS3_GX_GP_H
#define PS3_GX_GP_H

#include <stdbool.h>

#include <dolphin/types.h>

// CP registers.
#define CP_MATINDEX_A 0x30
#define CP_MATINDEX_B 0x40
#define CP_VCD_LO 0x50
#define CP_VCD_HI 0x60
#define CP_VAT_A 0x70
#define CP_VAT_B 0x80
#define CP_VAT_C 0x90
#define CP_ARRAY_BASE 0xA0
#define CP_ARRAY_STRIDE 0xB0

// XF memory and registers (addresses 0x1000 and up).
#define XF_MEM_SIZE 0x680
#define XF_NRM_MTX 0x400
#define XF_DUALTEX_MTX 0x500
#define XF_LIGHTS 0x600
#define XF_REG_BASE 0x1000
#define XF_REG_COUNT 0x58

// BP registers with side effects.
#define BP_PE_DONE 0x45
#define BP_PE_TOKEN 0x47
#define BP_PE_TOKEN_INT 0x48
#define BP_EFB_COPY 0x52
#define BP_LOAD_TLUT1 0x65
#define BP_TEX_INVALIDATE 0x66
#define BP_TEV_REGISTER 0xE0
#define BP_MASK 0xFE

// Primitive types, from the draw opcodes 0x80-0xB8.
enum {
    GP_QUADS,
    GP_QUADS2,
    GP_TRIANGLES,
    GP_TRIANGLE_STRIP,
    GP_TRIANGLE_FAN,
    GP_LINES,
    GP_LINE_STRIP,
    GP_POINTS,
};

typedef struct GpState {
    u32 matindex_a, matindex_b;
    u32 vcd_lo, vcd_hi;
    u32 vat_a[8], vat_b[8], vat_c[8];
    u32 array_base[16], array_stride[16];
    u32 xf_mem[XF_MEM_SIZE];
    u32 xf_regs[XF_REG_COUNT];
    u32 bp[256];
    // BP 0xE0-0xE7 hold two register sets, chosen by bit 23: the TEV colour registers and the konst colours.
    u32 tev_color[4][2];
    u32 tev_konst[4][2];
    u32 bp_mask;
    // Bytes per vertex for each VAT; 0xFFFF when VCD or VAT changed since.
    u16 vtx_size[8];
    // Set on each write, for the backend to clear: one bit per BP register, per XF register, per 4 XF memory words.
    u32 bp_dirty[8];
    u32 xf_reg_dirty[3];
    u32 xf_mem_dirty[XF_MEM_SIZE / 128];
    // CP, XF and BP writes so far: a draw with none since the draw before has its registers.
    u32 writes;
    // Bumped when a value changes: BP (with the TEV register sets), XF registers but the matrix indices and the
    // projection, VCD and VAT. The backend keeps what it derived while the generations it read stay.
    u32 bp_gen, xf_gen, cp_gen;
} GpState;

typedef struct GpBackend {
    // data holds count vertices of gp_vertex_size(s, vat) bytes each, as the FIFO or display list had them.
    void (*draw)(GpState* s, int prim, int vat, u32 count, const u8* data);
    // BP_EFB_COPY was written; its value is in s->bp.
    void (*efb_copy)(GpState* s);
    // BP_LOAD_TLUT1 was written.
    void (*tlut_load)(GpState* s);
    void (*tex_invalidate)(GpState* s);
    // A draw done or token follows: the work so far must be finished.
    void (*finish)(GpState* s);
    // Show the display copy made to physical address xfb (gp_post_present).
    void (*present)(GpState* s, u32 xfb);
    // The CPU rewrote [paddr, paddr + size) (gp_post_mem_changed).
    void (*mem_changed)(GpState* s, u32 paddr, u32 size);
} GpBackend;

// Starts the GP thread over a ring of ring_size bytes (a power of two). backend lives until exit.
void gp_init(const GpBackend* backend, u32 ring_size);
u32 gp_vertex_size(GpState* s, int vat);
// Any thread: the GP thread presents xfb between two commands, or while it waits. A later post replaces one
// not yet run.
void gp_post_present(u32 xfb);
// Not on the GP thread: the GP thread lets the RSX finish its work and stops for good, waiting at most 2 s.
// Before the exit, so the process does not end with the RSX busy.
void gp_stop(void);
// The GX thread only: tells the backend, in stream order, that the CPU rewrote memory a texture may come from.
void gp_post_mem_changed(const void* p, u32 size);
// The registers, for tests and logs: read them only while the GP thread is idle (after gp_fifo_sync).
GpState* gp_get_state(void);

// Counters for the tests and the log.
typedef struct GpStats {
    u32 commands, draws, vertices, display_lists, unknown_opcodes, bad_addresses;
} GpStats;
void gp_get_stats(GpStats* out);

#endif
