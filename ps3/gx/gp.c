// The GP thread: decodes the FIFO the SDK's GX library writes, keeps the CP, XF and BP registers, runs display
// lists, and hands draws and EFB operations to the backend. Opcodes and register layout as in Dolphin.
#include "gp.h"

#include <string.h>

#include <dolphin/os.h>

#include <gx_pipe.h>

#include "../os/diag.h"
#include "../os/intr.h"
#include "../os/mem.h"
#include "../os/prof.h"
#include "../os/sync.h"
#include "../os/udplog.h"
#include "fifo.h"

#define OP_NOP 0x00
#define OP_LOAD_CP 0x08
#define OP_LOAD_XF 0x10
#define OP_LOAD_INDX_A 0x20
#define OP_LOAD_INDX_D 0x38
#define OP_CALL_DL 0x40
#define OP_METRICS 0x44
#define OP_INVL_VC 0x48
#define OP_LOAD_BP 0x61
// The port's own: a memory range the CPU rewrote (gp_post_mem_changed). The GameCube has no opcode 0x7E.
#define OP_PS3_MEM_CHANGED 0x7E
#define OP_DRAW 0x80

// PE token register, __peReg[7].
#define PE_TOKEN_PADDR 0x0C00100E

#define VTX_SIZE_UNKNOWN 0xFFFF

// Bytes per component: u8, s8, u16, s16, f32.
static const u8 comp_size[8] = { 1, 1, 2, 2, 4, 0, 0, 0 };
// Direct colours: RGB565, RGB8, RGBX8, RGBA4, RGBA6, RGBA8.
static const u8 color_size[8] = { 2, 3, 4, 2, 3, 4, 0, 0 };

static GpState state;
static const GpBackend* backend;
static GpStats stats;
// CP, XF and BP writes so far, and at the last draw: draws with none between them could be one RSX draw.
static u32 reg_writes, draw_writes;
static int draw_vat = -1;

// Ring source when end is NULL, else a display list in memory.
typedef struct Src {
    const u8* p;
    const u8* end;
} Src;

static inline u16 be16(const u8* p)
{
    return (u16) (p[0] << 8 | p[1]);
}

static inline u32 be32(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

// NULL past the end of a display list.
static inline const u8* take(Src* s, u32 n)
{
    const u8* p;

    if (s->end == NULL) {
        return gp_fifo_take(n);
    }
    if ((u32) (s->end - s->p) < n) {
        return NULL;
    }
    p = s->p;
    s->p += n;
    return p;
}

static void invalidate_vtx_sizes(void)
{
    memset(state.vtx_size, 0xFF, sizeof(state.vtx_size));
}

// Texture coordinate n's count bit and format, from VAT A to C.
static void tex_format(const GpState* s, int vat, int n, u32* count, u32* fmt)
{
    static const u8 word[8] = { 0, 1, 1, 1, 1, 2, 2, 2 };
    static const u8 shift[8] = { 21, 0, 9, 18, 27, 5, 14, 23 };
    u32 v = word[n] == 0 ? s->vat_a[vat] : word[n] == 1 ? s->vat_b[vat] : s->vat_c[vat];

    *count = (v >> shift[n]) & 1;
    *fmt = (v >> (shift[n] + 1)) & 7;
}

u32 gp_vertex_size(GpState* s, int vat)
{
    u32 lo = s->vcd_lo, hi = s->vcd_hi, a = s->vat_a[vat];
    u32 size = 0, t;
    int i;

    if (s->vtx_size[vat] != VTX_SIZE_UNKNOWN) {
        return s->vtx_size[vat];
    }
    // Matrix indices: one byte each, direct only.
    for (i = 0; i < 9; i++) {
        size += (lo >> i) & 1;
    }
    t = (lo >> 9) & 3;
    if (t == 1) {
        size += comp_size[(a >> 1) & 7] * ((a & 1) ? 3 : 2);
    } else if (t >= 2) {
        size += t - 1;
    }
    t = (lo >> 11) & 3;
    if (t != 0) {
        bool nbt = (a >> 9) & 1;

        if (t == 1) {
            size += comp_size[(a >> 10) & 7] * (nbt ? 9 : 3);
        } else {
            // NBT with NormalIndex3 has an index for each of the three vectors.
            size += (t - 1) * ((nbt && (a >> 31)) ? 3 : 1);
        }
    }
    for (i = 0; i < 2; i++) {
        t = (lo >> (13 + 2 * i)) & 3;
        if (t == 1) {
            size += color_size[(a >> (14 + 4 * i)) & 7];
        } else if (t >= 2) {
            size += t - 1;
        }
    }
    for (i = 0; i < 8; i++) {
        u32 count, fmt;

        t = (hi >> (2 * i)) & 3;
        if (t == 1) {
            tex_format(s, vat, i, &count, &fmt);
            size += comp_size[fmt] * (count ? 2 : 1);
        } else if (t >= 2) {
            size += t - 1;
        }
    }
    s->vtx_size[vat] = (u16) size;
    return size;
}

static void cp_write(u8 addr, u32 v)
{
    u32 n = addr & 0xF;

    reg_writes++;
    switch (addr & 0xF0) {
    case CP_MATINDEX_A:
        state.matindex_a = v;
        break;
    case CP_MATINDEX_B:
        state.matindex_b = v;
        break;
    case CP_VCD_LO:
        if (state.vcd_lo != v) {
            state.vcd_lo = v;
            state.cp_gen++;
            invalidate_vtx_sizes();
        }
        break;
    case CP_VCD_HI:
        if (state.vcd_hi != v) {
            state.vcd_hi = v;
            state.cp_gen++;
            invalidate_vtx_sizes();
        }
        break;
    case CP_VAT_A:
    case CP_VAT_B:
    case CP_VAT_C:
        if (n < 8) {
            u32* vat = (addr & 0xF0) == CP_VAT_A ? state.vat_a : (addr & 0xF0) == CP_VAT_B ? state.vat_b
                                                                                            : state.vat_c;
            if (vat[n] != v) {
                vat[n] = v;
                state.cp_gen++;
                invalidate_vtx_sizes();
            }
        }
        break;
    case CP_ARRAY_BASE:
        state.array_base[n] = v;
        break;
    case CP_ARRAY_STRIDE:
        state.array_stride[n] = v & 0xFF;
        break;
    }
}

static void xf_write(u32 addr, u32 v)
{
    reg_writes++;
    if (addr < XF_MEM_SIZE) {
        state.xf_mem[addr] = v;
        if (state.xf_mem_dirty_lo > addr) {
            state.xf_mem_dirty_lo = addr;
        }
        if (state.xf_mem_dirty_hi < addr + 1) {
            state.xf_mem_dirty_hi = addr + 1;
        }
    } else if (addr - XF_REG_BASE < XF_REG_COUNT) {
        u32 i = addr - XF_REG_BASE;

        // The matrix indices (0x18, 0x19) and the projection (0x20-0x26) feed only the vertices and constants.
        if (state.xf_regs[i] != v && (i < 0x18 || i > 0x26 || (i >= 0x1A && i <= 0x1F))) {
            state.xf_gen++;
        }
        state.xf_regs[i] = v;
        state.xf_reg_dirty[i >> 5] |= 1u << (i & 31);
    }
}

// LOAD_INDX_A-D: XF words from arrays 12-15 (position, normal and texture matrices, lights).
static void load_indexed(u8 op, u32 v)
{
    u32 array = 12 + ((op - OP_LOAD_INDX_A) >> 3);
    u32 index = v >> 16, count = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    const u8* src = OSPhysicalToCached(state.array_base[array] + index * state.array_stride[array]);
    u32 i;

    if (src == NULL) {
        stats.bad_addresses++;
        return;
    }
    for (i = 0; i < count; i++) {
        xf_write(addr + i, be32(src + 4 * i));
    }
}

static void finish_work(void)
{
    if (backend->finish != NULL) {
        ps3_diag.gp_site = "finish";
        backend->finish(&state);
    }
}

static void bp_write(u32 v)
{
    u32 reg = v >> 24;
    u32 value = v & 0xFFFFFF;

    reg_writes++;
    if (reg == BP_MASK) {
        state.bp_mask = value;
        return;
    }
    // The mask applies to the next write only.
    value = (state.bp[reg] & ~state.bp_mask) | (value & state.bp_mask);
    state.bp_mask = 0xFFFFFF;
    // A TEV register write that leaves bp[reg] as it was also leaves its register set as it was.
    if (state.bp[reg] != value) {
        state.bp_gen++;
    }
    state.bp[reg] = value;
    state.bp_dirty[reg >> 5] |= 1u << (reg & 31);

    switch (reg) {
    case BP_PE_DONE:
        if ((value & 0xFF) == 2) {
            finish_work();
            ps3_intr_raise(PS3_INTR_PE_FINISH);
        }
        break;
    case BP_PE_TOKEN:
        *ps3_mmio16(PE_TOKEN_PADDR) = (u16) value;
        break;
    case BP_PE_TOKEN_INT:
        finish_work();
        *ps3_mmio16(PE_TOKEN_PADDR) = (u16) value;
        ps3_intr_raise(PS3_INTR_PE_TOKEN);
        break;
    case BP_EFB_COPY:
        if (backend->efb_copy != NULL) {
            ps3_diag.gp_site = "EFB copy";
            backend->efb_copy(&state);
        }
        break;
    case BP_LOAD_TLUT1:
        if (backend->tlut_load != NULL) {
            ps3_diag.gp_site = "TLUT load";
            backend->tlut_load(&state);
        }
        break;
    case BP_TEX_INVALIDATE:
        if (backend->tex_invalidate != NULL) {
            backend->tex_invalidate(&state);
        }
        break;
    default:
        if (reg - BP_TEV_REGISTER < 8) {
            u32 i = (reg - BP_TEV_REGISTER) >> 1, half = reg & 1;

            if ((value >> 23) & 1) {
                state.tev_konst[i][half] = value;
            } else {
                state.tev_color[i][half] = value;
            }
        }
        break;
    }
}

static void run_list(u32 paddr, u32 size);

// Runs one command. Returns false at the end of a display list or on a truncated one.
static bool run_command(Src* s, bool in_list)
{
    const u8* p = take(s, 1);
    u8 op;

    if (p == NULL) {
        return false;
    }
    op = p[0];
    ps3_diag.gp_op = op;
    ps3_diag.gp_site = "commands";
    stats.commands++;
    if (op & OP_DRAW) {
        int vat = op & 7;
        u32 count, size;

        if ((p = take(s, 2)) == NULL) {
            return false;
        }
        count = be16(p);
        size = gp_vertex_size(&state, vat);
        if (count == 0 || size == 0) {
            return true;
        }
        if ((p = take(s, count * size)) == NULL) {
            return false;
        }
        stats.draws++;
        stats.vertices += count;
        if (reg_writes == draw_writes && vat == draw_vat) {
            ps3_prof.batchable++;
        }
        draw_writes = reg_writes;
        draw_vat = vat;
        ps3_diag.gp_site = "draw";
        backend->draw(&state, (op >> 3) & 7, vat, count, p);
        return true;
    }
    switch (op) {
    case OP_NOP:
    case OP_METRICS:
    case OP_INVL_VC:
        break;
    case OP_LOAD_CP:
        if ((p = take(s, 5)) == NULL) {
            return false;
        }
        cp_write(p[0], be32(p + 1));
        break;
    case OP_LOAD_XF: {
        u32 head, count, addr, i;

        if ((p = take(s, 4)) == NULL) {
            return false;
        }
        head = be32(p);
        count = (head >> 16) + 1;
        addr = head & 0xFFFF;
        for (i = 0; i < count; i++) {
            if ((p = take(s, 4)) == NULL) {
                return false;
            }
            xf_write(addr + i, be32(p));
        }
        break;
    }
    case OP_LOAD_INDX_A:
    case OP_LOAD_INDX_A + 0x08:
    case OP_LOAD_INDX_A + 0x10:
    case OP_LOAD_INDX_D:
        if ((p = take(s, 4)) == NULL) {
            return false;
        }
        load_indexed(op, be32(p));
        break;
    case OP_CALL_DL: {
        u32 addr, size;

        if ((p = take(s, 8)) == NULL) {
            return false;
        }
        addr = be32(p);
        size = be32(p + 4);
        // The GameCube does not nest display lists.
        if (!in_list) {
            run_list(addr, size);
        }
        break;
    }
    case OP_LOAD_BP:
        if ((p = take(s, 4)) == NULL) {
            return false;
        }
        bp_write(be32(p));
        break;
    case OP_PS3_MEM_CHANGED:
        if ((p = take(s, 8)) == NULL) {
            return false;
        }
        if (backend->mem_changed != NULL) {
            ps3_diag.gp_site = "memory changed";
            backend->mem_changed(&state, be32(p), be32(p + 4));
        }
        break;
    default:
        // An unknown opcode loses the stream position, as on the GameCube; count it and go on.
        stats.unknown_opcodes++;
        break;
    }
    return true;
}

static void run_list(u32 paddr, u32 size)
{
    Src s;

    s.p = OSPhysicalToCached(paddr);
    if (s.p == NULL) {
        stats.bad_addresses++;
        return;
    }
    s.end = s.p + size;
    stats.display_lists++;
    while (run_command(&s, true)) {
    }
}

static volatile int present_pending;
static volatile u32 present_xfb;

// GP thread only.
static void run_present(void)
{
    if (__atomic_exchange_n(&present_pending, 0, __ATOMIC_ACQ_REL) && backend->present != NULL) {
        ps3_diag.gp_site = "present";
        backend->present(&state, __atomic_load_n(&present_xfb, __ATOMIC_ACQUIRE));
    }
}

void gp_post_present(u32 xfb)
{
    __atomic_store_n(&present_xfb, xfb, __ATOMIC_RELEASE);
    __atomic_store_n(&present_pending, 1, __ATOMIC_RELEASE);
    gp_fifo_kick();
}

void gp_post_mem_changed(const void* p, u32 size)
{
    u32 paddr = OSCachedToPhysical((void*) p);

    __GXPipe_u8(OP_PS3_MEM_CHANGED);
    __GXPipe_u32(paddr);
    __GXPipe_u32(size);
}

static void gp_main(void* arg)
{
    Src ring = { NULL, NULL };

    ps3_log("gp: thread running\n");
    for (;;) {
        run_command(&ring, false);
        gp_fifo_release();
        if (__atomic_load_n(&present_pending, __ATOMIC_RELAXED)) {
            run_present();
        }
    }
}

void gp_init(const GpBackend* b, u32 ring_size)
{
    if (backend != NULL) {
        return;
    }
    backend = b;
    state.bp_mask = 0xFFFFFF;
    state.xf_mem_dirty_lo = XF_MEM_SIZE;
    invalidate_vtx_sizes();
    ps3_log("gp: ring of %u bytes\n", ring_size);
    gp_fifo_init(ring_size);
    gp_fifo_set_idle_work(run_present);
    ps3_log("gp: ring ready, starting the GP thread\n");
    ps3_log("gp: thread start returned %d\n", ps_thread_start(gp_main, NULL, "GP"));
}

GpState* gp_get_state(void)
{
    return &state;
}

void gp_get_stats(GpStats* out)
{
    *out = stats;
}
