// The GP thread: decodes the FIFO the SDK's GX library writes, keeps the CP, XF and BP registers, runs display
// lists, and hands draws and EFB operations to the backend. Opcodes and register layout as in Dolphin.
#include "gp.h"

#include <stdint.h>
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
// The port's own, opcodes the GameCube does not have: a copy into game memory (gp_post_copy), and a memory range
// the CPU rewrote (gp_post_mem_changed).
#define OP_PS3_COPY 0x7D
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
// The writes and the VAT at the last draw: draws with no write between them could be one RSX draw.
static u32 draw_writes;
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

u32 gp_list_draws(GpState* s, const u8* p, u32 size, GpListDraw* out, u32 max)
{
    const u8* end = p + size;
    u32 n = 0;

    while (p < end) {
        u8 op = *p++;
        u32 count, bytes;

        if (op == OP_NOP) {
            continue;
        }
        if (!(op & OP_DRAW) || end - p < 2) {
            return 0;
        }
        count = be16(p);
        p += 2;
        bytes = count * gp_vertex_size(s, op & 7);
        if ((u32) (end - p) < bytes) {
            return 0;
        }
        if (bytes != 0) {
            if (n == max) {
                return 0;
            }
            out[n].prim = (op >> 3) & 7;
            out[n].vat = op & 7;
            out[n].count = (u16) count;
            out[n].data = p;
            n++;
        }
        p += bytes;
    }
    return n;
}

// The GpState counters register writes change, in registers over a run of commands (run_regs): on the PPE a value
// stored by one command and loaded again by the next stalls the pipeline (load-hit-store).
typedef struct Gens {
    u32 writes, bp_gen, xf_gen, cp_gen, tev_gen, bp_mask;
} Gens;

static inline void gens_load(Gens* g)
{
    g->writes = state.writes;
    g->bp_gen = state.bp_gen;
    g->xf_gen = state.xf_gen;
    g->cp_gen = state.cp_gen;
    g->tev_gen = state.tev_gen;
    g->bp_mask = state.bp_mask;
}

static inline void gens_store(const Gens* g)
{
    state.writes = g->writes;
    state.bp_gen = g->bp_gen;
    state.xf_gen = g->xf_gen;
    state.cp_gen = g->cp_gen;
    state.tev_gen = g->tev_gen;
    state.bp_mask = g->bp_mask;
}

static inline void cp_write(Gens* g, u8 addr, u32 v)
{
    u32 n = addr & 0xF;

    g->writes++;
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
            g->cp_gen++;
            invalidate_vtx_sizes();
        }
        break;
    case CP_VCD_HI:
        if (state.vcd_hi != v) {
            state.vcd_hi = v;
            g->cp_gen++;
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
                g->cp_gen++;
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

// The caller marks the register dirty (xf_load).
static inline void xf_reg_write(Gens* g, u32 i, u32 v)
{
    // The matrix indices (0x18, 0x19) and the projection (0x20-0x26) feed only the vertices and constants.
    if (state.xf_regs[i] != v && (i < 0x18 || i > 0x26 || (i >= 0x1A && i <= 0x1F))) {
        g->xf_gen++;
        // The texgen count and the texgens' projection bits feed the TEV config.
        if (i >= 0x3F && i <= 0x47) {
            g->tev_gen++;
        }
    }
    state.xf_regs[i] = v;
}

// Sets bits first to last of a dirty bitmap, one store per word.
static inline void mark_dirty(u32* dirty, u32 first, u32 last)
{
    u32 w;

    for (w = first >> 5; w <= last >> 5; w++) {
        u32 lo = w == first >> 5 ? first & 31 : 0, hi = w == last >> 5 ? last & 31 : 31;

        dirty[w] |= (2u << hi) - (1u << lo);
    }
}

// n big-endian words from p, any alignment, to d. With aligned loads: on the PPE a load that crosses a 32-byte
// boundary stalls, and the FIFO's XF words are misaligned as often as not.
static inline __attribute__((always_inline)) void copy_be32(u32* d, const u8* p, u32 n)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    u32 sh = (u32) ((uintptr_t) p & 3) * 8, prev, a, b;
    const u32* w = (const u32*) (p - ((uintptr_t) p & 3));

    if (sh == 0) {
        for (; n != 0; n--) {
            *d++ = *w++;
        }
        return;
    }
    // Two words a turn, fewer moves of the carried word. The last load is of the word that holds the last byte.
    prev = *w++;
    for (; n >= 2; n -= 2, w += 2, d += 2) {
        a = w[0];
        b = w[1];
        d[0] = prev << sh | a >> (32 - sh);
        d[1] = a << sh | b >> (32 - sh);
        prev = b;
    }
    if (n != 0) {
        *d = prev << sh | *w >> (32 - sh);
    }
#else
    for (; n != 0; n--, p += 4) {
        *d++ = be32(p);
    }
#endif
}

// count XF words from p (big-endian, any alignment) from XF address addr on: one write command. Inline, so the
// caller's Gens stays in registers.
static inline __attribute__((always_inline)) void xf_load(Gens* g, u32 addr, const u8* p, u32 count)
{
    u32 first = XF_REG_COUNT, last = 0;

    g->writes++;
    if (addr < XF_MEM_SIZE) {
        u32 n = XF_MEM_SIZE - addr < count ? XF_MEM_SIZE - addr : count;

        copy_be32(&state.xf_mem[addr], p, n);
        // A dirty bit per 4 words.
        mark_dirty(state.xf_mem_dirty, addr >> 2, (addr + n - 1) >> 2);
        addr += n;
        p += 4 * n;
        count -= n;
    }
    for (; count != 0; count--, addr++, p += 4) {
        u32 r = addr - XF_REG_BASE;

        if (r < XF_REG_COUNT) {
            xf_reg_write(g, r, be32(p));
            first = r < first ? r : first;
            last = r;
        }
    }
    if (first < XF_REG_COUNT) {
        mark_dirty(state.xf_reg_dirty, first, last);
    }
}

// LOAD_INDX_A-D: XF words from arrays 12-15 (position, normal and texture matrices, lights).
static void load_indexed(Gens* g, u8 op, u32 v)
{
    u32 array = 12 + ((op - OP_LOAD_INDX_A) >> 3);
    u32 index = v >> 16, count = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    const u8* src = OSPhysicalToCached(state.array_base[array] + index * state.array_stride[array]);

    if (src == NULL) {
        stats.bad_addresses++;
        return;
    }
    xf_load(g, addr, src, count);
}

static void finish_work(void)
{
    if (backend->finish != NULL) {
        ps3_diag.gp_site = "finish";
        backend->finish(&state);
    }
}

// The BP registers the backend's TEV config and values read: genmode, indirect stages, orders, stages, the TEV
// colour and konst registers, fog, the z texture, swap tables and konst selections.
static bool tev_bp(u32 reg)
{
    return reg == 0x00 || reg == 0x43 || (reg >= 0x10 && reg <= 0x1F) || (reg >= 0x28 && reg <= 0x2F) ||
           (reg >= 0xC0 && reg <= 0xE7) || (reg >= 0xEE && reg <= 0xF2) || (reg >= 0xF4 && reg <= 0xFD);
}

// The BP registers whose write does more than store the value (bp_write).
static inline bool bp_has_effect(u32 reg)
{
    return reg == BP_PE_DONE || reg == BP_PE_TOKEN || reg == BP_PE_TOKEN_INT || reg == BP_EFB_COPY ||
           reg == BP_LOAD_TLUT1 || reg == BP_TEX_INVALIDATE;
}

// Stores a BP write under the mask: its register.
static inline u32 bp_store(Gens* g, u32 v)
{
    u32 reg = v >> 24;
    u32 value = v & 0xFFFFFF;

    g->writes++;
    if (reg == BP_MASK) {
        g->bp_mask = value;
        return reg;
    }
    // The mask applies to the next write only.
    value = (state.bp[reg] & ~g->bp_mask) | (value & g->bp_mask);
    g->bp_mask = 0xFFFFFF;
    // A TEV register write that leaves bp[reg] as it was also leaves its register set as it was.
    if (state.bp[reg] != value) {
        g->bp_gen++;
        if (tev_bp(reg)) {
            g->tev_gen++;
        }
    }
    state.bp[reg] = value;
    if (reg - BP_TEV_REGISTER < 8) {
        u32 i = (reg - BP_TEV_REGISTER) >> 1, half = reg & 1;

        if ((value >> 23) & 1) {
            state.tev_konst[i][half] = value;
        } else {
            state.tev_color[i][half] = value;
        }
    }
    return reg;
}

static void bp_write(Gens* g, u32 v)
{
    u32 reg = bp_store(g, v), value = state.bp[reg];

    if (!bp_has_effect(reg)) {
        return;
    }
    // The backend reads the registers and the counters.
    gens_store(g);
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
    }
}

static void run_list(u32 paddr, u32 size);

// Copies posted (GX thread) and made (GP thread); copy_waiting with the SEQ_CST pair as fifo.c's cons_waiting.
static u32 copies_posted;
static u32 copies_done;
static int copy_waiting;
static PsMutex copy_lock = PS_MUTEX_INIT;
static PsCond copy_cond = PS_COND_INIT;

static void run_copy(u32 dst_paddr, u32 size, const void* src)
{
    void* dst = OSPhysicalToCached(dst_paddr);

    ps3_diag.gp_site = "copy";
    if (dst != NULL) {
        memcpy(dst, src, size);
        if (backend->mem_changed != NULL) {
            backend->mem_changed(&state, dst_paddr, size);
        }
    } else {
        stats.bad_addresses++;
    }
    __atomic_store_n(&copies_done, copies_done + 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&copy_waiting, __ATOMIC_SEQ_CST)) {
        ps_mutex_lock(&copy_lock);
        ps_cond_broadcast(&copy_cond);
        ps_mutex_unlock(&copy_lock);
    }
}

// The register commands whole in [p, end) in one pass, up to another command, a BP write with an effect or
// GP_FIFO_RELEASE_STEP bytes: the position after them.
static const u8* run_regs(const u8* p, const u8* end)
{
    const u8* stop = (u32) (end - p) > GP_FIFO_RELEASE_STEP ? p + GP_FIFO_RELEASE_STEP : end;
    u32 n = 0;
    u8 op = 0;
    Gens g;

    gens_load(&g);
    while (p < stop) {
        u8 next = *p;

        if (next == OP_LOAD_BP) {
            if (end - p < 5 || bp_has_effect(p[1])) {
                break;
            }
            bp_store(&g, be32(p + 1));
            p += 5;
        } else if (next == OP_LOAD_XF) {
            u32 head, count;

            if (end - p < 5) {
                break;
            }
            head = be32(p + 1);
            count = (head >> 16) + 1;
            if ((u32) (end - p) - 5 < 4 * count) {
                break;
            }
            xf_load(&g, head & 0xFFFF, p + 5, count);
            p += 5 + 4 * count;
        } else if (next == OP_LOAD_CP) {
            if (end - p < 6) {
                break;
            }
            cp_write(&g, p[1], be32(p + 2));
            p += 6;
        } else if (next == OP_NOP || next == OP_METRICS || next == OP_INVL_VC) {
            p++;
        } else {
            break;
        }
        op = next;
        n++;
    }
    if (n != 0) {
        gens_store(&g);
        stats.commands += n;
        ps3_diag.gp_op = op;
        ps3_diag.gp_site = "commands";
    }
    return p;
}

// Runs one command, or a run of register commands. Returns false at the end of a display list or on a truncated one.
static bool run_command(Src* s, bool in_list)
{
    const u8* p;
    u8 op;
    Gens g;

    if (s->end == NULL) {
        p = run_regs(gp_fifo_rp, gp_fifo_avail);
        if (p != gp_fifo_rp) {
            gp_fifo_rp = p;
            return true;
        }
    } else {
        p = run_regs(s->p, s->end);
        if (p != s->p) {
            s->p = p;
            return true;
        }
    }
    if ((p = take(s, 1)) == NULL) {
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
        if (state.writes == draw_writes && vat == draw_vat) {
            ps3_prof.batchable++;
        }
        draw_writes = state.writes;
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
        gens_load(&g);
        cp_write(&g, p[0], be32(p + 1));
        gens_store(&g);
        break;
    case OP_LOAD_XF: {
        u32 head, count;

        if ((p = take(s, 4)) == NULL) {
            return false;
        }
        head = be32(p);
        count = (head >> 16) + 1;
        if ((p = take(s, 4 * count)) == NULL) {
            return false;
        }
        gens_load(&g);
        xf_load(&g, head & 0xFFFF, p, count);
        gens_store(&g);
        break;
    }
    case OP_LOAD_INDX_A:
    case OP_LOAD_INDX_A + 0x08:
    case OP_LOAD_INDX_A + 0x10:
    case OP_LOAD_INDX_D:
        if ((p = take(s, 4)) == NULL) {
            return false;
        }
        gens_load(&g);
        load_indexed(&g, op, be32(p));
        gens_store(&g);
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
        gens_load(&g);
        bp_write(&g, be32(p));
        gens_store(&g);
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
    case OP_PS3_COPY:
        if ((p = take(s, 16)) == NULL) {
            return false;
        }
        run_copy(be32(p), be32(p + 4), (const void*) (uintptr_t) ((u64) be32(p + 8) << 32 | be32(p + 12)));
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
    if (backend->call_list != NULL) {
        ps3_diag.gp_site = "display list";
        if (backend->call_list(&state, paddr, s.p, size, &stats.draws, &stats.vertices)) {
            return;
        }
    }
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

static volatile int stop_asked;
static int stopped;
static PsMutex stop_lock = PS_MUTEX_INIT;
static PsCond stop_cond = PS_COND_INIT;

// GP thread: once gp_stop asks, the RSX finishes what it has and the thread waits for the process to end.
static void run_stop(void)
{
    if (!__atomic_load_n(&stop_asked, __ATOMIC_ACQUIRE)) {
        return;
    }
    ps3_diag.gp_site = "stop";
    if (backend->finish != NULL) {
        backend->finish(&state);
    }
    ps_mutex_lock(&stop_lock);
    stopped = 1;
    ps_cond_broadcast(&stop_cond);
    for (;;) {
        ps_cond_wait(&stop_cond, &stop_lock);
    }
}

static void run_idle(void)
{
    run_present();
    run_stop();
}

void gp_stop(void)
{
    const char* self = ps_thread_name();
    int i;

    if (backend == NULL || (self != NULL && strcmp(self, "GP") == 0)) {
        return;
    }
    __atomic_store_n(&stop_asked, 1, __ATOMIC_RELEASE);
    gp_fifo_kick();
    ps_mutex_lock(&stop_lock);
    for (i = 0; i < 20 && !stopped; i++) {
        ps_cond_timedwait(&stop_cond, &stop_lock, 100000);
    }
    ps_mutex_unlock(&stop_lock);
    ps3_log("gp: %s\n", stopped ? "stopped, the RSX is idle" : "did not stop in 2 s");
}

void gp_post_mem_changed(const void* p, u32 size)
{
    u32 paddr = OSCachedToPhysical((void*) p);

    __GXPipe_u8(OP_PS3_MEM_CHANGED);
    __GXPipe_u32(paddr);
    __GXPipe_u32(size);
}

u32 gp_post_copy(void* dst, const void* src, u32 size)
{
    u64 s = (u64) (uintptr_t) src;

    __GXPipe_u8(OP_PS3_COPY);
    __GXPipe_u32(OSCachedToPhysical(dst));
    __GXPipe_u32(size);
    __GXPipe_u32((u32) (s >> 32));
    __GXPipe_u32((u32) s);
    return ++copies_posted;
}

void gp_wait_copy(u32 n)
{
    if ((s32) (__atomic_load_n(&copies_done, __ATOMIC_SEQ_CST) - n) >= 0) {
        return;
    }
    // An unpublished copy would never run.
    __GXPipeFlush();
    ps_mutex_lock(&copy_lock);
    __atomic_store_n(&copy_waiting, 1, __ATOMIC_SEQ_CST);
    while ((s32) (__atomic_load_n(&copies_done, __ATOMIC_SEQ_CST) - n) < 0) {
        ps_cond_wait(&copy_cond, &copy_lock);
    }
    __atomic_store_n(&copy_waiting, 0, __ATOMIC_SEQ_CST);
    ps_mutex_unlock(&copy_lock);
}

// A/B: the GP thread below the main thread (1100), so the other port threads' wakeups take the GP's hardware
// thread, not main's.
#define GP_PRIORITY 1001
#define GP_PRIORITY_LOW 1200

static void gp_main(void* arg)
{
    Src ring = { NULL, NULL };
    unsigned low = 0;

    ps3_log("gp: thread running\n");
    for (;;) {
        if ((ps3_diag.gx_ab & 134217728) != low) {
            low = ps3_diag.gx_ab & 134217728;
            ps3_log("gp: priority %d: %d\n", low ? GP_PRIORITY_LOW : GP_PRIORITY,
                    ps_thread_set_priority(low ? GP_PRIORITY_LOW : GP_PRIORITY));
        }
        run_command(&ring, false);
        gp_fifo_release_step();
        if (__atomic_load_n(&present_pending, __ATOMIC_RELAXED)) {
            run_present();
        }
        if (__atomic_load_n(&stop_asked, __ATOMIC_RELAXED)) {
            run_stop();
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
    invalidate_vtx_sizes();
    ps3_log("gp: ring of %u bytes\n", ring_size);
    gp_fifo_init(ring_size);
    gp_fifo_set_idle_work(run_idle);
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
