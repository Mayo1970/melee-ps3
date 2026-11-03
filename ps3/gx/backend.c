// The GP backend on the RSX. Register meanings as Dolphin's BPMemory/XFMemory; state mapping after DolphinPS3's
// VertexManager. Runs on the GP thread only.
#include "backend.h"

#include <malloc.h>
#include <string.h>

#include <dolphin/os.h>

#include "../os/diag.h"
#include "../os/mem.h"
#include "../os/prof.h"
#include "../os/udplog.h"
#include "fpgen.h"
#include "rsxdev.h"
#include "tevgen.h"
#include "texdec.h"
#include "vpgen.h"
#include "vtx.h"
#include "vxjob.h"
#include "xfgen.h"

#define EFB_PITCH (EFB_WIDTH * 4)
#define FP_CACHE 2048
#define VP_CACHE 1024
#define LAYOUTS 64
#define TEX_CACHE 512
#define TEX_BUCKETS 1024
#define TEX_VERSIONS 4
#define XFBS 4
#define XFB_MAX_HEIGHT 576
#define MAX_TEX_SIZE 1024
// Texture data hashed in full up to this size, sampled above it.
#define TEX_HASH_FULL 4096

enum {
    U_LOGIC_OP = 1 << 0,
    U_ALPHA_TWO = 1 << 1,
    U_TEV_COMPARE = 1 << 2,
    U_INDIRECT = 1 << 3,
    U_DST_ALPHA = 1 << 5,
    U_DEPTH_COPY = 1 << 6,
    U_COPY_FORMAT = 1 << 7,
    U_TEX_FORMAT = 1 << 8,
    U_BUMP_RAS = 1 << 9,
    U_TEX7_MTXIDX = 1 << 10,
};

typedef struct FpEntry {
    bool used;
    u32 hash;
    TevConfig cfg;
    void* mem;
    u32 offset;
    u16 regs, texcoords, texcoords4, words;
    bool depth;
} FpEntry;

typedef struct VpEntry {
    bool used;
    s16 slot; // first RSX program slot, -1 when not loaded
    u32 hash;
    XfKey key;
    u32 (*ucode)[4];
    u16 n, regs;
    u32 inputs, outputs;
} VpEntry;

typedef struct TexEntry {
    bool used, efb;
    s16 next; // chain of the entries at one address hash (tex_bucket)
    u32 addr, fmt, width, height, levels, tlut_fmt;
    u32 bytes; // GX texture data read, all levels
    u32 hash, tlut_hash;
    // The ps3_mem_stamp the data was hashed at: hashed again only after a write to it.
    u32 stamp;
    void* mem;
    u32 offset, pitch;
    u32 frame;
} TexEntry;

typedef struct Xfb {
    u32 addr;
    void* mem;
    u32 offset;
    u16 width, height;
} Xfb;

typedef struct CopyVertex {
    float pos[4];
    float tc[2];
} CopyVertex;

static GxRsxStats stats;
static bool initialized;
// The first draws and copies go to the log, to find a hang on hardware.
static u32 trace_left = 120;

static void* efb_color;
static void* efb_depth;
static DevTarget efb_target;
// The RSX state matches the last GX draw; a copy pass clears these.
static bool efb_bound, textures_bound;
static const TexEntry* bound[8];

// The constants as the RSX has them (float bits), and the rows changed since the last upload.
static float vc[C_COUNT][4];
static u32 vc_changed[(C_COUNT + 31) / 32];
static bool vc_valid;

static FpEntry fp_cache[FP_CACHE];
static FpProg fp_scratch;
static u32 fp_words[FP_MAXI * 8];
static const FpEntry* fp_current;

// The fragment program of each TEV key (tev_key): the register words a TEV config is made from, less the values,
// then the words of the values in the program's code, if any (tev_key_code).
#define TEV_KEY_WORDS (53 + 21)
#define TEV_KEY_BITS 10
#define TEV_KEYS (1 << TEV_KEY_BITS)

typedef struct TevKeyEntry {
    bool used;
    u8 n;
    u16 mask;
    // The value rows in the code (build_tev): this entry only says so, the key with their words names the program.
    u16 code;
    u32 hash;
    const FpEntry* f;
    u32 key[TEV_KEY_WORDS];
} TevKeyEntry;

static TevKeyEntry tev_keys[TEV_KEYS];

static VpEntry vp_cache[VP_CACHE];
static VpProg vp_scratch;
static const VpEntry* vp_current;
static VpEntry copy_vp;
// The program starting at each slot. Programs are placed one after the other, so one never overlaps another.
static VpEntry* vp_slots[DEV_VP_SLOTS];
static u32 vp_slot_next;
// The XMB overlay; the presents after it ends still reload the programs, as its last flips may come late.
#define OVERLAY_TAIL 30
static volatile bool overlay_on;
static u32 overlay_presents;
static void (*screen_fn)(GxRsxScreen* out);

static VtxLayout layouts[LAYOUTS];
static int nlayouts, layout_next;
// Each layout's attributes for dev_attribs: format words, and offsets in the vertex.
static struct {
    u32 format[16], offset[16];
} layout_attribs[LAYOUTS];

static TexEntry tex_cache[TEX_CACHE];
// The cache entries by address: the first entry of each chain, -1 when none.
static s16 tex_bucket[TEX_BUCKETS];
// Per GX map: the registers of the last lookup and its entry, kept while no TLUT load or memory write comes.
static struct {
    u32 regs[5], tlut_loads;
    DevTexture d;
    TexEntry* e;
} tex_memo[8];
static u32 tlut_loads;
static u32* tex_temp;
static u32* tex_swz;
static u8 tmem_tlut[0x80000];
static u32 frame;

static Xfb xfbs[XFBS];
static int xfb_next;

// Consecutive GX draws with no register write between them and the same VAT: one indexed RSX draw. The vertices
// and then the indices go into one ring reservation; the indices wait in batch_idx.
#define BATCH_INDICES 0x8000
#define BATCH_VERTICES 0xFFFF

static struct {
    bool open;
    u32 family; // DEV_PRIM_TRIANGLES, DEV_PRIM_LINES or DEV_PRIM_POINTS
    u32 writes;
    int vat;
    const VtxLayout* l;
    u8* base;
    u32 io, avail, verts, nidx;
} batch;
static u16 batch_idx[BATCH_INDICES];

// What the last draw derived from the registers, valid while the GpState generations it read stay.
static struct {
    bool layout_ok, tev_ok, vp_ok, state_ok, tex_ok, consts_ok;
    u32 layout_cp;
    int layout_vat;
    VtxLayout* layout;
    u32 tev_gen, tev_tco;
    const FpEntry* f;
    float tev[XF_TEV_CONSTS][4];
    u16 tev_mask;
    // The register words the TEV value rows were converted from (tev_values): K0-K3, PREV and REG0-REG2, fog.
    u32 tev_raw[8][2], fog_raw[5];
    u16 tev_raw_ok;
    u32 vp_xf;
    u16 vp_attrs, vp_tev;
    bool vp_pos3, vp_nospec, vp_olddepth;
    VpEntry* v;
    // The RSX raster state as apply_state left it, its result, and the register words it read (state_same).
    u32 state_bp, state_xf;
    bool state_pass, state_olddepth;
    u32 state_src[17];
    // The textures as bind_textures left them: the units the program samples and their register words.
    u32 tex_bp, tex_stamp, tex_tluts, tex_ab, tex_used;
    u32 tex_regs[8][5];
} memo;

// The A/B bits for one draw setup, read once, so the memos and the work they guard see the same modes.
static unsigned draw_ab;

static void unsupported(u32 bit, const char* what)
{
    if (!(stats.unsupported & bit)) {
        stats.unsupported |= bit;
        OSReport("GX RSX: %s is not drawn as on the GameCube\n", what);
    }
}

static u32 hash_bytes(const void* data, u32 size, u32 h)
{
    const u8* p = data;

    while (size-- != 0) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

// As hash_bytes a word at a time, a quarter of the multiplies: data 4-byte aligned, size a multiple of 4.
static u32 hash_words(const void* data, u32 size, u32 h)
{
    const u32* p = data;

    for (size /= 4; size != 0; size--) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

static float u2f(u32 v)
{
    float f;

    memcpy(&f, &v, 4);
    return f;
}

// Programs

static void* local_copy(const void* src, u32 bytes, u32* offset)
{
    void* mem = dev_local_alloc(64, bytes, offset);

    if (mem != NULL) {
        dev_write_local(mem, src, bytes);
    }
    return mem;
}

static void clear_fp_cache(void)
{
    int i;

    dev_finish();
    for (i = 0; i < FP_CACHE; i++) {
        if (fp_cache[i].used) {
            dev_local_free(fp_cache[i].mem);
            fp_cache[i].used = false;
        }
    }
    memset(tev_keys, 0, sizeof(tev_keys));
    fp_current = NULL;
    memo.tev_ok = false;
    memo.f = NULL;
}

static FpEntry* fp_build(FpEntry* e, int slots)
{
    int words = slots < 0 ? -1 : fp_ucode(&fp_scratch, fp_words, sizeof(fp_words) / 4);

    if (words <= 0) {
        OSReport("GX RSX: fragment program generation failed (error %d)\n", fp_scratch.err);
        return NULL;
    }
    e->mem = local_copy(fp_words, (u32) words * 4, &e->offset);
    if (e->mem == NULL) {
        return NULL;
    }
    e->regs = (u16) fp_num_regs(&fp_scratch);
    e->words = (u16) words;
    e->texcoords = fp_scratch.texcoords;
    e->texcoords4 = fp_scratch.texcoords4;
    e->depth = fp_scratch.depth;
    e->used = true;
    stats.fragment_programs++;
    return e;
}

static const FpEntry* fp_get(const TevConfig* cfg)
{
    u32 h = hash_bytes(cfg, sizeof(*cfg), 2166136261u), i;

    for (i = 0; i < FP_CACHE; i++) {
        FpEntry* e = &fp_cache[(h + i) % FP_CACHE];

        if (!e->used) {
            if (i > FP_CACHE / 2) {
                break;
            }
            e->hash = h;
            e->cfg = *cfg;
            return fp_build(e, tev_gen(cfg, &fp_scratch));
        }
        if (e->hash == h && memcmp(&e->cfg, cfg, sizeof(*cfg)) == 0) {
            return e;
        }
    }
    clear_fp_cache();
    return fp_get(cfg);
}

static void vp_recent_add(VpEntry* recent[4], VpEntry* e)
{
    recent[3] = recent[2];
    recent[2] = recent[1];
    recent[1] = recent[0];
    recent[0] = e;
}

// xf_make_key clears the whole key, padding too, so keys compare and hash as words.
_Static_assert(sizeof(XfKey) % 4 == 0 && _Alignof(XfKey) >= 4, "XfKey compares as words");

static bool xf_key_equal(const XfKey* a, const XfKey* b)
{
    const u32* p = (const u32*) a;
    const u32* q = (const u32*) b;
    u32 i;

    for (i = 0; i < sizeof(XfKey) / 4; i++) {
        if (p[i] != q[i]) {
            return false;
        }
    }
    return true;
}

static VpEntry* vp_get(const XfKey* key)
{
    // The last programs found, newest first: entries are never freed.
    static VpEntry* recent[4];
    u32 h, i;

    for (i = 0; i < 4 && recent[i] != NULL; i++) {
        if (xf_key_equal(&recent[i]->key, key)) {
            VpEntry* e = recent[i];

            for (; i > 0; i--) {
                recent[i] = recent[i - 1];
            }
            recent[0] = e;
            return e;
        }
    }
    h = hash_words(key, sizeof(*key), 2166136261u);
    for (i = 0; i < VP_CACHE; i++) {
        VpEntry* e = &vp_cache[(h + i) % VP_CACHE];

        if (!e->used) {
            if (xf_gen(key, &vp_scratch) < 0) {
                OSReport("GX RSX: vertex program generation failed (error %d)\n", vp_scratch.err);
                return NULL;
            }
            e->ucode = malloc(16u * vp_scratch.n);
            if (e->ucode == NULL) {
                return NULL;
            }
            memcpy(e->ucode, vp_scratch.w, 16u * vp_scratch.n);
            e->n = (u16) vp_scratch.n;
            e->slot = -1;
            e->regs = (u16) (vp_scratch.max_temp + 1);
            e->inputs = vp_scratch.input_mask;
            e->outputs = vp_scratch.output_mask;
            e->hash = h;
            e->key = *key;
            e->used = true;
            stats.vertex_programs++;
            vp_recent_add(recent, e);
            return e;
        }
        if (e->hash == h && xf_key_equal(&e->key, key)) {
            vp_recent_add(recent, e);
            return e;
        }
    }
    OSReport("GX RSX: vertex program cache full\n");
    return NULL;
}

static void use_vp(VpEntry* v)
{
    u32 at = vp_slot_next, i;

    if (v == vp_current) {
        return;
    }
    vp_current = v;
    if (v->slot >= 0 && !(ps3_diag.gx_ab & 2)) {
        dev_vertex_program(NULL, v->n, (u32) v->slot, v->inputs, v->outputs, v->regs);
        return;
    }
    if (at + v->n > DEV_VP_SLOTS) {
        at = 0;
    }
    for (i = at; i < at + v->n && i < DEV_VP_SLOTS; i++) {
        if (vp_slots[i] != NULL) {
            vp_slots[i]->slot = -1;
            vp_slots[i] = NULL;
        }
    }
    // A longer program runs over the present pass's slots, which loads its own each time: it stays unloaded.
    if (v->n <= DEV_VP_SLOTS) {
        v->slot = (s16) at;
        vp_slots[at] = v;
    }
    vp_slot_next = at + v->n;
    if (ps3_diag.gx_ab & 128) {
        dev_barrier();
    }
    dev_vertex_program((const u32(*)[4]) v->ucode, v->n, at, v->inputs, v->outputs, v->regs);
    stats.vp_loads++;
}

// No program stays loaded: each one goes up again at its next use.
static void vp_forget(void)
{
    u32 i;

    for (i = 0; i < DEV_VP_SLOTS; i++) {
        if (vp_slots[i] != NULL) {
            vp_slots[i]->slot = -1;
            vp_slots[i] = NULL;
        }
    }
    vp_slot_next = 0;
}

static void use_fp(const FpEntry* f)
{
    if (f != fp_current) {
        // A/B: the RSX finishes its draws before a fragment program change.
        if (ps3_diag.gx_ab & 16384) {
            dev_barrier();
        }
        dev_fragment_program(f->offset, f->regs, f->texcoords, f->texcoords4, f->depth);
        fp_current = f;
    }
}

// TEV

// KSEL swap table n as (r, g, b, a) sources.
static void swap_table(const GpState* s, int n, u8 out[4])
{
    u32 a = s->bp[0xF6 + 2 * n], b = s->bp[0xF7 + 2 * n];

    out[0] = (u8) (a & 3);
    out[1] = (u8) ((a >> 2) & 3);
    out[2] = (u8) (b & 3);
    out[3] = (u8) ((b >> 2) & 3);
}

static float s11(u32 v)
{
    s32 x = (s32) (v << 21) >> 21;
    return (float) x / 255.0f;
}

// A TEV colour or konst register as (r, g, b, a).
static void tev_reg(const u32 half[2], bool konst, float out[4])
{
    if (konst) {
        out[0] = (float) (half[0] & 0xFF) / 255.0f;
        out[3] = (float) ((half[0] >> 12) & 0xFF) / 255.0f;
        out[2] = (float) (half[1] & 0xFF) / 255.0f;
        out[1] = (float) ((half[1] >> 12) & 0xFF) / 255.0f;
    } else {
        out[0] = s11(half[0]);
        out[3] = s11(half[0] >> 12);
        out[2] = s11(half[1]);
        out[1] = s11(half[1] >> 12);
    }
}

static void fog_values(const GpState* s, float param[4], float color[4], bool* ortho)
{
    u32 f0 = s->bp[0xEE], f3 = s->bp[0xF1], fc = s->bp[0xF2];
    float a = u2f((f0 >> 19 & 1) << 31 | ((f0 >> 11) & 0xFF) << 23 | (f0 & 0x7FF) << 12);
    float c = u2f((f3 >> 19 & 1) << 31 | ((f3 >> 11) & 0xFF) << 23 | (f3 & 0x7FF) << 12);

    *ortho = (f3 >> 20) & 1;
    if (*ortho) {
        param[0] = a / 16777216.0f;
        param[1] = 0.0f;
        param[2] = 0.0f;
    } else {
        param[0] = a * 16777216.0f;
        param[1] = (float) (s->bp[0xEF] & 0xFFFFFF);
        param[2] = 1.0f / (float) (1u << (s->bp[0xF0] & 0x1F));
    }
    param[3] = c;
    color[0] = (float) ((fc >> 16) & 0xFF) / 255.0f;
    color[1] = (float) ((fc >> 8) & 0xFF) / 255.0f;
    color[2] = (float) (fc & 0xFF) / 255.0f;
    color[3] = 0.0f;
}

// The TEV config for the current BP registers, with the per-draw values on free texcoord interpolants. Returns the
// value rows with no free interpolant, which went into the config (the rows of mask).
static u16 build_tev(const GpState* s, int numtexgens, u32 tco, TevConfig* cfg, float vals[XF_TEV_CONSTS][4],
                     u16* mask)
{
    u8 tables[4][4];
    float kval[4][4], reg[4][4], fog_param[4], fog_color[4];
    u32 regs_read, konst_read;
    int i, slot = numtexgens;
    bool ortho;
    u16 code = 0;

    memset(cfg, 0, sizeof(*cfg));
    *mask = 0;
    cfg->nstages = (u8) (((s->bp[0x00] >> 10) & 0xF) + 1);
    for (i = 0; i < 4; i++) {
        swap_table(s, i, tables[i]);
    }
    if ((s->bp[0x00] >> 16) & 7) {
        unsupported(U_INDIRECT, "indirect texturing");
    }
    for (i = 0; i < cfg->nstages; i++) {
        TevStage* st = &cfg->st[i];
        u32 cc = s->bp[0xC0 + 2 * i], ac = s->bp[0xC1 + 2 * i];
        u32 tref = s->bp[0x28 + i / 2] >> ((i & 1) ? 12 : 0);
        u32 ksel = s->bp[0xF6 + i / 2], chan;

        st->col.d = cc & 0xF;
        st->col.c = (cc >> 4) & 0xF;
        st->col.b = (cc >> 8) & 0xF;
        st->col.a = (cc >> 12) & 0xF;
        st->col.bias = (cc >> 16) & 3;
        st->col.op = (cc >> 18) & 1;
        st->col.clamp = (cc >> 19) & 1;
        st->col.scale = (cc >> 20) & 3;
        st->col.dest = (cc >> 22) & 3;
        st->alp.d = (ac >> 4) & 7;
        st->alp.c = (ac >> 7) & 7;
        st->alp.b = (ac >> 10) & 7;
        st->alp.a = (ac >> 13) & 7;
        st->alp.bias = (ac >> 16) & 3;
        st->alp.op = (ac >> 18) & 1;
        st->alp.clamp = (ac >> 19) & 1;
        st->alp.scale = (ac >> 20) & 3;
        st->alp.dest = (ac >> 22) & 3;
        if (st->col.bias == 3 || st->alp.bias == 3) {
            unsupported(U_TEV_COMPARE, "a TEV compare stage");
            if (st->col.bias == 3) {
                st->col.bias = 0;
            }
            if (st->alp.bias == 3) {
                st->alp.bias = 0;
            }
        }
        if (s->bp[0x10 + i] != 0) {
            unsupported(U_INDIRECT, "indirect texturing");
        }
        st->tex = (tref >> 6) & 1 ? (s8) (tref & 7) : -1;
        st->texcoord = (u8) ((tref >> 3) & 7);
        st->proj = st->tex >= 0 && ((s->xf_regs[0x40 + st->texcoord] >> 1) & 1);
        chan = (tref >> 7) & 7;
        st->ras = chan == 0 ? RAS_COL0 : chan == 1 ? RAS_COL1 : RAS_ZERO;
        if (chan == 5 || chan == 6) {
            unsupported(U_BUMP_RAS, "the bump alpha colour channel");
        }
        memcpy(st->ras_swap, tables[ac & 3], 4);
        memcpy(st->tex_swap, tables[(ac >> 2) & 3], 4);
        st->kcsel = (u8) ((ksel >> ((i & 1) ? 14 : 4)) & 0x1F);
        st->kasel = (u8) ((ksel >> ((i & 1) ? 19 : 9)) & 0x1F);
    }
    for (i = 0; i < 4; i++) {
        tev_reg(s->tev_konst[i], true, kval[i]);
        tev_reg(s->tev_color[i], false, reg[i]);
        cfg->kin[i] = -1;
        cfg->rin[i] = -1;
    }
    cfg->fog_pin = -1;
    cfg->fog_cin = -1;
    regs_read = tev_regs_read(cfg);
    konst_read = tev_konst_read(cfg);
    for (i = 0; i < 4; i++) {
        if (!((konst_read >> i) & 1)) {
            continue;
        }
        if (slot < 8) {
            cfg->kin[i] = (s8) (FIN_TC0 + slot++);
            memcpy(vals[i], kval[i], 16);
            *mask |= (u16) (1u << i);
        } else {
            memcpy(cfg->kval[i], kval[i], 16);
            code |= (u16) (1u << i);
        }
    }
    for (i = 0; i < 4; i++) {
        if (!((regs_read >> i) & 1)) {
            continue;
        }
        if (slot < 8) {
            cfg->rin[i] = (s8) (FIN_TC0 + slot++);
            memcpy(vals[4 + i], reg[i], 16);
            *mask |= (u16) (1u << (4 + i));
        } else {
            memcpy(cfg->reg[i], reg[i], 16);
            code |= (u16) (1u << (4 + i));
        }
    }
    cfg->fog = (u8) ((s->bp[0xF1] >> 21) & 7);
    if (cfg->fog == 1 || cfg->fog == 3) {
        cfg->fog = FOG_NONE;
    }
    if (cfg->fog != FOG_NONE) {
        fog_values(s, fog_param, fog_color, &ortho);
        cfg->fog_ortho = ortho;
        if (slot < 8) {
            cfg->fog_pin = (s8) (FIN_TC0 + slot++);
            memcpy(vals[8], fog_param, 16);
            *mask |= 1u << 8;
        } else {
            memcpy(cfg->fog_param, fog_param, 16);
            code |= 1u << 8;
        }
        if (slot < 8) {
            cfg->fog_cin = (s8) (FIN_TC0 + slot++);
            memcpy(vals[9], fog_color, 16);
            *mask |= 1u << 9;
        } else {
            memcpy(cfg->fog_color, fog_color, 16);
            code |= 1u << 9;
        }
    }
    if (((s->bp[0xF5] >> 2) & 3) == ZT_ADD || ((s->bp[0xF5] >> 2) & 3) == ZT_REPLACE) {
        cfg->ztex_op = (u8) ((s->bp[0xF5] >> 2) & 3);
        cfg->ztex_type = (u8) (s->bp[0xF5] & 3);
        cfg->ztex_bias = (float) (s->bp[0xF4] & 0xFFFFFF);
    }
    if (tco != 0) {
        cfg->tco_kind = (u8) (tco >> 16);
        cfg->tco_mask = (u8) tco;
        cfg->tco = 1.0f / (float) ((tco >> 8) & 0xFF);
    }
    return code;
}

// The words of the registers build_tev makes a config from, less the TEV values: n words into key, returned. Equal
// keys give equal configs while the values are on interpolants (tev_key_code otherwise).
static u32 tev_key(const GpState* s, u32 numtexgens, u32 tco, u32 key[TEV_KEY_WORDS])
{
    u32 gen = s->bp[0x00], nstages = ((gen >> 10) & 0xF) + 1, n = 5, swaps = 0, proj = 0, ind = 0, i;
    u32 zop = (s->bp[0xF5] >> 2) & 3;

    for (i = 0; i < 8; i++) {
        swaps |= (s->bp[0xF6 + i] & 0xF) << (4 * i);
    }
    for (i = 0; i < nstages; i++) {
        u32 tref = s->bp[0x28 + i / 2] >> ((i & 1) ? 12 : 0);

        key[n++] = s->bp[0xC0 + 2 * i];
        key[n++] = s->bp[0xC1 + 2 * i];
        ind |= s->bp[0x10 + i];
        if ((tref >> 6) & 1) {
            proj |= ((s->xf_regs[0x40 + ((tref >> 3) & 7)] >> 1) & 1) << i;
        }
    }
    // The order and konst selections of each stage pair: one half only for an odd last stage.
    for (i = 0; i < (nstages + 1) / 2; i++) {
        bool both = 2 * i + 1 < nstages;

        key[n++] = s->bp[0x28 + i] & (both ? 0x3FF3FFu : 0x3FFu);
        key[n++] = s->bp[0xF6 + i] & (both ? 0xFFFFF0u : 0x3FF0u);
    }
    key[0] = (gen >> 10 & 0xF) | (gen >> 16 & 7) << 4 | numtexgens << 7 | (s->bp[0xF1] >> 20 & 0xF) << 11 |
             (u32) (ind != 0) << 15 | proj << 16;
    key[1] = swaps;
    // The z texture, as build_tev reads it: nothing while it is off.
    key[2] = zop == ZT_ADD || zop == ZT_REPLACE ? s->bp[0xF5] & 0xF : 0;
    key[3] = key[2] != 0 ? s->bp[0xF4] & 0xFFFFFF : 0;
    key[4] = tco;
    return n;
}

// After the n words of tev_key, the register words of the value rows in code (build_tev's result): the new count.
static u32 tev_key_code(const GpState* s, u16 code, u32 key[TEV_KEY_WORDS], u32 n)
{
    u32 i;

    for (i = 0; i < 8; i++) {
        if ((code >> i) & 1) {
            const u32* half = i < 4 ? s->tev_konst[i] : s->tev_color[i - 4];

            key[n++] = half[0];
            key[n++] = half[1];
        }
    }
    if (code & 0x300) {
        for (i = 0; i < 5; i++) {
            key[n++] = s->bp[0xEE + i];
        }
    }
    return n;
}

static u32 tev_key_hash(const u32* key, u32 n)
{
    u32 h = 0, i;

    for (i = 0; i < n; i++) {
        h = ((h << 7) | (h >> 25)) ^ key[i];
    }
    return h * 2654435761u;
}

// The entry of key, or a free one to fill; a full table starts again.
static TevKeyEntry* tev_lookup(const u32* key, u32 n, u32 h)
{
    u32 i, k;

    for (i = 0; i < TEV_KEYS / 2; i++) {
        TevKeyEntry* e = &tev_keys[((h >> (32 - TEV_KEY_BITS)) + i) & (TEV_KEYS - 1)];

        if (!e->used) {
            return e;
        }
        if (e->hash != h || e->n != n) {
            continue;
        }
        for (k = 0; k < n && e->key[k] == key[k]; k++) {
        }
        if (k == n) {
            return e;
        }
    }
    OSReport("GX RSX: TEV key table full, cleared\n");
    memset(tev_keys, 0, sizeof(tev_keys));
    return &tev_keys[h >> (32 - TEV_KEY_BITS)];
}

// The TEV value rows of mask from their registers, converted again only when a register word changed; the other
// rows are 0. A changed row sends the constants again.
static void tev_values(const GpState* s, u16 mask)
{
    u32 i;

    if (mask != memo.tev_mask) {
        memset(memo.tev, 0, sizeof(memo.tev));
        memo.tev_mask = mask;
        memo.tev_raw_ok = 0;
        memo.consts_ok = false;
    }
    for (i = 0; i < 8; i++) {
        const u32* half = i < 4 ? s->tev_konst[i] : s->tev_color[i - 4];
        u32* raw = memo.tev_raw[i];

        if (!((mask >> i) & 1) || (((memo.tev_raw_ok >> i) & 1) && raw[0] == half[0] && raw[1] == half[1])) {
            continue;
        }
        raw[0] = half[0];
        raw[1] = half[1];
        tev_reg(half, i < 4, memo.tev[i]);
        memo.tev_raw_ok |= (u16) (1u << i);
        memo.consts_ok = false;
    }
    if (mask & 0x300) {
        u32* raw = memo.fog_raw;

        if (!(memo.tev_raw_ok & 0x100) || memcmp(raw, &s->bp[0xEE], sizeof(memo.fog_raw)) != 0) {
            float param[4], color[4];
            bool ortho;

            memcpy(raw, &s->bp[0xEE], sizeof(memo.fog_raw));
            fog_values(s, param, color, &ortho);
            if (mask & 0x100) {
                memcpy(memo.tev[8], param, 16);
            }
            if (mask & 0x200) {
                memcpy(memo.tev[9], color, 16);
            }
            memo.tev_raw_ok |= 0x100;
            memo.consts_ok = false;
        }
    }
}

#ifdef GX_TEV_CHECK
// Host check: each draw setup builds the config as before and compares the program, mask and values.
static void tev_check(const GpState* s, u32 numtexgens, u32 tco)
{
    static TevConfig cfg;
    static float vals[XF_TEV_CONSTS][4];
    static u32 checks, errors;
    u16 mask;

    memset(vals, 0, sizeof(vals));
    build_tev(s, (int) numtexgens, tco, &cfg, vals, &mask);
    checks++;
    if (fp_get(&cfg) != memo.f || mask != memo.tev_mask || memcmp(vals, memo.tev, sizeof(vals)) != 0) {
        if (errors++ < 20) {
            ps3_log("tev check: setup %u differs: mask %#x against %#x, program %s\n", checks, mask, memo.tev_mask,
                    fp_get(&cfg) == memo.f ? "same" : "differs");
        }
    }
    if (checks % 100000 == 0) {
        ps3_log("tev check: %u setups, %u differ, %u fragment programs\n", checks, errors, stats.fragment_programs);
    }
}
#endif

// The fragment program and TEV values of the current registers. The program is found by a word compare of tev_key,
// with the words of the values in code if the config has some; build_tev and the config hash run for a new key only.
static void tev_setup(const GpState* s, u32 numtexgens, u32 tco)
{
    static TevConfig cfg;
    static float vals[XF_TEV_CONSTS][4];
    u32 key[TEV_KEY_WORDS], n = tev_key(s, numtexgens, tco, key), h = tev_key_hash(key, n);
    TevKeyEntry* e = tev_lookup(key, n, h);
    const FpEntry* f;
    bool with_code = false;
    u16 mask;

    ps3_prof.tev_setups++;
    if ((s->bp[0x00] >> 16) & 7) {
        ps3_prof.indirect_tevs++;
    }
    if (e->used && e->code != 0) {
        n = tev_key_code(s, e->code, key, n);
        h = tev_key_hash(key, n);
        e = tev_lookup(key, n, h);
        with_code = true;
    }
    if (e->used) {
        f = e->f;
        mask = e->mask;
    } else {
        u16 code;

        ps3_prof.tev_builds++;
        code = build_tev(s, (int) numtexgens, tco, &cfg, vals, &mask);
        f = fp_get(&cfg);
        if (f == NULL) {
            memo.tev_ok = false;
            memo.f = NULL;
            return;
        }
        // fp_get may have cleared the key table: e is then a free entry like any other.
        if (code != 0 && !with_code) {
            e->used = true;
            e->code = code;
            e->n = (u8) n;
            e->hash = h;
            memcpy(e->key, key, n * 4);
            n = tev_key_code(s, code, key, n);
            h = tev_key_hash(key, n);
            e = tev_lookup(key, n, h);
        }
        e->used = true;
        e->code = 0;
        e->n = (u8) n;
        e->mask = mask;
        e->hash = h;
        e->f = f;
        memcpy(e->key, key, n * 4);
    }
    // The textures follow the program's config (bind_textures).
    if (f != memo.f) {
        memo.f = f;
        memo.tex_ok = false;
    }
    tev_values(s, mask);
    memo.tev_gen = s->tev_gen;
    memo.tev_tco = tco;
    memo.tev_ok = true;
}

// Raster state

static u16 blend_factor(u32 f, bool is_dst, bool efb_alpha)
{
    switch (f) {
    case 0:
        return DEV_ZERO;
    case 1:
        return DEV_ONE;
    case 2:
        return is_dst ? DEV_SRC_COLOR : DEV_DST_COLOR;
    case 3:
        return is_dst ? DEV_ONE_MINUS_SRC_COLOR : DEV_ONE_MINUS_DST_COLOR;
    case 4:
        return DEV_SRC_ALPHA;
    case 5:
        return DEV_ONE_MINUS_SRC_ALPHA;
    case 6:
        return efb_alpha ? DEV_DST_ALPHA : DEV_ONE;
    default:
        return efb_alpha ? DEV_ONE_MINUS_DST_ALPHA : DEV_ZERO;
    }
}

// Marks GX compare c of 8-bit alpha against ref as a constant (*k) of value *v when every alpha gives one result.
static void alpha_const(u32 c, u32 ref, bool* k, bool* v)
{
    if ((c == 6 && ref == 0) || (c == 3 && ref == 255)) {
        *k = *v = true;
    } else if ((c == 1 && ref == 0) || (c == 4 && ref == 255)) {
        *k = true;
        *v = false;
    }
}

// The single-compare forms of the GX alpha test. False when no pixel can pass.
static bool apply_alpha_test(const GpState* s)
{
    u32 at = s->bp[0xF3], ref0 = at & 0xFF, ref1 = (at >> 8) & 0xFF;
    u32 c0 = (at >> 16) & 7, c1 = (at >> 19) & 7, logic = (at >> 22) & 3;
    bool k0 = c0 == 0 || c0 == 7, k1 = c1 == 0 || c1 == 7;
    bool v0 = c0 == 7, v1 = c1 == 7;
    bool old = (draw_ab & 524288) != 0;

    // Not in the old A/B mode: always-true or always-false compares, and two equal halves, reduce too. Melee's
    // translucent materials use alpha > 0 AND alpha > 0 with depth writes: with no test, clear pixels wrote depth.
    if (!old) {
        alpha_const(c0, ref0, &k0, &v0);
        alpha_const(c1, ref1, &k1, &v1);
        if (!k0 && c0 == c1 && ref0 == ref1) {
            if (logic >= 2) {
                dev_alpha_test(false, 0, 0);
                return logic == 3;
            }
            // x AND true, x OR false: x.
            k1 = true;
            v1 = logic == 0;
        }
    }
    if (k0 && k1) {
        bool pass = logic == 0 ? v0 && v1 : logic == 1 ? v0 || v1 : logic == 2 ? v0 != v1 : v0 == v1;

        dev_alpha_test(false, 0, 0);
        return pass;
    }
    if (logic == 0 && (k0 || k1)) {
        if ((k0 && !v0) || (k1 && !v1)) {
            return false;
        }
        dev_alpha_test(true, DEV_FUNC_NEVER + (k0 ? c1 : c0), (u8) (k0 ? ref1 : ref0));
        return true;
    }
    if (logic == 1 && (k0 || k1)) {
        if ((k0 && v0) || (k1 && v1)) {
            dev_alpha_test(false, 0, 0);
            return true;
        }
        dev_alpha_test(true, DEV_FUNC_NEVER + (k0 ? c1 : c0), (u8) (k0 ? ref1 : ref0));
        return true;
    }
    if (!old && (k0 || k1)) {
        // x XOR false and x XNOR true are x; the other two invert x (7 - c in GX's order).
        bool keep = (logic == 2) != (k0 ? v0 : v1);
        u32 c = k0 ? c1 : c0;

        dev_alpha_test(true, DEV_FUNC_NEVER + (keep ? c : 7 - c), (u8) (k0 ? ref1 : ref0));
        return true;
    }
    unsupported(U_ALPHA_TWO, "a two-compare alpha test");
    dev_alpha_test(false, 0, 0);
    return true;
}

static bool efb_has_alpha(const GpState* s)
{
    return (s->bp[0x43] & 7) == 1;
}

static void scissor_offset(const GpState* s, int* ox, int* oy)
{
    *ox = (int) (s->bp[0x59] & 0x3FF) * 2;
    *oy = (int) ((s->bp[0x59] >> 10) & 0x3FF) * 2;
}

static bool apply_state(const GpState* s)
{
    const u32* x = s->xf_regs;
    u32 zmode = s->bp[0x40], cm0 = s->bp[0x41], cull = (s->bp[0x00] >> 14) & 3;
    float scale[4], offset[4];
    int ox, oy, x0, y0, x1, y1;
    bool efb_alpha = efb_has_alpha(s);

    if (!efb_bound) {
        dev_target(&efb_target);
        efb_bound = true;
    }
    scissor_offset(s, &ox, &oy);
    // The XF viewport less the scissor offset. The program gives -z (z + w in the old A/B mode, xfgen.h), so
    // depth is GX's far + range * z / w either way.
    scale[0] = u2f(x[0x1A]);
    scale[1] = u2f(x[0x1B]);
    scale[2] = (memo.vp_olddepth ? 1.0f : -1.0f) * u2f(x[0x1C]) / 16777215.0f;
    scale[3] = 0.0f;
    // GX samples pixels at 7/12 of a pixel, the RSX at 1/2: as Dolphin, the image moves 1/12 pixel up and left.
    offset[0] = u2f(x[0x1D]) - (float) ox - 1.0f / 12.0f;
    offset[1] = u2f(x[0x1E]) - (float) oy - 1.0f / 12.0f;
    offset[2] = (u2f(x[0x1F]) - (memo.vp_olddepth ? u2f(x[0x1C]) : 0.0f)) / 16777215.0f;
    offset[3] = 0.0f;
    dev_viewport(scale, offset);
    x0 = (int) ((s->bp[0x20] >> 12) & 0x7FF) - ox;
    y0 = (int) (s->bp[0x20] & 0x7FF) - oy;
    x1 = (int) ((s->bp[0x21] >> 12) & 0x7FF) - ox + 1;
    y1 = (int) (s->bp[0x21] & 0x7FF) - oy + 1;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > EFB_WIDTH ? EFB_WIDTH : x1;
    y1 = y1 > EFB_HEIGHT ? EFB_HEIGHT : y1;
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    dev_scissor((u32) x0, (u32) y0, (u32) (x1 - x0), (u32) (y1 - y0));
    if (zmode & 1) {
        dev_depth(true, DEV_FUNC_NEVER + ((zmode >> 1) & 7), (zmode >> 4) & 1);
    } else {
        // As Dolphin: no depth writes without the depth test.
        dev_depth(false, DEV_FUNC_NEVER + 7, false);
    }
    if (cm0 & (1u << 11)) {
        dev_blend(true, DEV_ONE, DEV_ONE, DEV_EQ_REVERSE_SUBTRACT);
    } else if (cm0 & 1) {
        dev_blend(true, blend_factor((cm0 >> 8) & 7, false, efb_alpha), blend_factor((cm0 >> 5) & 7, true, efb_alpha),
                  DEV_EQ_ADD);
    } else {
        dev_blend(false, DEV_ONE, DEV_ZERO, DEV_EQ_ADD);
    }
    if ((cm0 & 2) && !(cm0 & 1) && ((cm0 >> 12) & 0xF) != 3) {
        unsupported(U_LOGIC_OP, "a logic op");
    }
    if ((s->bp[0x42] >> 8) & 1) {
        unsupported(U_DST_ALPHA, "the constant destination alpha");
    }
    dev_color_mask((cm0 >> 3) & 1, ((cm0 >> 4) & 1) && efb_alpha);
    // Hardware cull values: 1 culls back faces, 2 front faces. GX front faces are clockwise on the screen.
    dev_cull(cull == 2 || cull == 3, cull == 1 || cull == 3);
    dev_line_width((float) (s->bp[0x22] & 0xFF) / 6.0f);
    dev_point_size((float) ((s->bp[0x22] >> 8) & 0xFF) / 6.0f);
    return apply_alpha_test(s);
}

// What apply_state reads besides the viewport depth mode: these BP registers, the XF viewport (0x1A-0x1F), the cull
// bits of genmode and the alpha test A/B bit. memo.state_src holds them in this order.
static const u8 state_bps[9] = { 0x20, 0x21, 0x22, 0x40, 0x41, 0x42, 0x43, 0x59, 0xF3 };

static bool state_same(const GpState* s)
{
    const u32* m = memo.state_src;
    int i;

    for (i = 0; i < 9; i++) {
        if (m[i] != s->bp[state_bps[i]]) {
            return false;
        }
    }
    for (i = 0; i < 6; i++) {
        if (m[9 + i] != s->xf_regs[0x1A + i]) {
            return false;
        }
    }
    return m[15] == (s->bp[0x00] & 0xC000) && m[16] == (draw_ab & 524288);
}

static void state_save(const GpState* s)
{
    u32* m = memo.state_src;
    int i;

    for (i = 0; i < 9; i++) {
        m[i] = s->bp[state_bps[i]];
    }
    for (i = 0; i < 6; i++) {
        m[9 + i] = s->xf_regs[0x1A + i];
    }
    m[15] = s->bp[0x00] & 0xC000;
    m[16] = draw_ab & 524288;
}

#ifdef GX_STATE_CHECK
// Check build (host): a draw that keeps the raster state runs apply_state again, which must change nothing.
static void state_check(const GpState* s)
{
    static u8 before[256], after[256];
    static u32 checks, errors;
    u32 n = dev_debug_raster(before, sizeof(before));
    bool pass = apply_state(s);

    checks++;
    if (dev_debug_raster(after, sizeof(after)) != n || memcmp(before, after, n) != 0 || pass != memo.state_pass) {
        if (errors++ < 20) {
            ps3_log("state check: kept state %u differs (pass %d against %d)\n", checks, pass, memo.state_pass);
        }
    }
    if (checks % 100000 == 0) {
        ps3_log("state check: %u kept states, %u differ\n", checks, errors);
    }
}
#endif

// Constants

// Words, not floats: a float load of a value just stored as an integer waits for the store (load-hit-store).
// Returns 1 when the row changed; the caller gathers the bits, as vc_changed's would wait for the store too.
static inline u64 set_row(int r, u32 x, u32 y, u32 z, u32 w)
{
    u32* d = (u32*) vc[r];

    if (d[0] != x || d[1] != y || d[2] != z || d[3] != w) {
        d[0] = x;
        d[1] = y;
        d[2] = z;
        d[3] = w;
        return 1;
    }
    return 0;
}

// The DQ and TEV rows' bits in update_constants: rows 192 to 255, one u64.
#define C_HI 192
_Static_assert(C_DQ >= C_HI && C_COUNT <= C_HI + 64 && C_HI % 32 == 0, "the DQ and TEV rows fit one u64");

// Each run of changed rows in one upload, in row order; hi adds bits for rows C_HI and up. Runs go on across words.
static void upload_changed(u64 hi)
{
    int run = -1;
    u32 w;
    bool barrier = (ps3_diag.gx_ab & 64) != 0;

    for (w = 0; w < (C_COUNT + 31) / 32; w++) {
        u32 x = vc_changed[w], pos = 0, m;

        if (w >= C_HI / 32) {
            x |= (u32) (hi >> (32 * (w - C_HI / 32)));
        }
        while (pos < 32) {
            if (run < 0) {
                m = x >> pos;
                if (m == 0) {
                    break;
                }
                pos += (u32) __builtin_ctz(m);
                run = (int) (w * 32 + pos);
            }
            m = ~x >> pos;
            if (m == 0) {
                break;
            }
            pos += (u32) __builtin_ctz(m);
            if (barrier) {
                dev_barrier();
                barrier = false;
            }
            dev_vp_constants((u32) run, w * 32 + pos - (u32) run, vc[run]);
            run = -1;
        }
    }
    if (run >= 0) {
        if (barrier) {
            dev_barrier();
        }
        dev_vp_constants((u32) run, (u32) (C_COUNT - run), vc[run]);
    }
    memset(vc_changed, 0, sizeof(vc_changed));
}

static void update_constants(GpState* s, const VtxLayout* l, float tev[XF_TEV_CONSTS][4], u16 tev_mask)
{
    const u32* q = (const u32*) l->dq;
    u32 mem = 0, i;
    u64 hi;
    bool xf_dirty;

    if (ps3_diag.gx_ab & 4) {
        vc_valid = false;
    }
    for (i = 0; i < XF_MEM_SIZE / 128; i++) {
        mem |= s->xf_mem_dirty[i];
    }
    xf_dirty = !vc_valid || mem != 0 || (s->xf_reg_dirty[0] & XF_CONST_REGS_0) ||
               (s->xf_reg_dirty[1] & XF_CONST_REGS_1);
    // The same layout and TEV values as the last upload: the DQ and TEV rows are on the RSX.
    if (!xf_dirty && memo.consts_ok) {
        return;
    }
    if (!vc_valid) {
        // The shadow starts unlike every value, so all of it goes up once.
        memset(vc, 0xFF, sizeof(vc));
        xf_constants(s, NULL, NULL, vc, vc_changed);
        vc_valid = true;
    } else if (xf_dirty) {
        ps3_prof.xf_reloads++;
        xf_constants(s, s->xf_mem_dirty, s->xf_reg_dirty, vc, vc_changed);
    }
    memset(s->xf_mem_dirty, 0, sizeof(s->xf_mem_dirty));
    memset(s->xf_reg_dirty, 0, sizeof(s->xf_reg_dirty));
    hi = set_row(C_DQ, q[0], q[1], q[2], q[3]) << (C_DQ - C_HI);
    hi |= set_row(C_DQ + 1, q[4], q[5], q[6], q[7]) << (C_DQ + 1 - C_HI);
    hi |= set_row(C_DQ + 2, q[8], q[9], 0, 0) << (C_DQ + 2 - C_HI);
    for (i = 0; i < XF_TEV_CONSTS; i++) {
        if ((tev_mask >> i) & 1) {
            const u32* t = (const u32*) tev[i];

            hi |= set_row(C_TEV + (int) i, t[0], t[1], t[2], t[3]) << (C_TEV + i - C_HI);
        }
    }
    upload_changed(hi);
    memo.consts_ok = true;
}

// Vertex layouts

static VtxLayout* get_layout(const GpState* s, int vat)
{
    int i, k;

    for (i = 0; i < nlayouts; i++) {
        VtxLayout* l = &layouts[i];

        if (l->vcd_lo == s->vcd_lo && l->vcd_hi == s->vcd_hi && l->vat_a == s->vat_a[vat] &&
            l->vat_b == s->vat_b[vat] && l->vat_c == s->vat_c[vat]) {
            return l;
        }
    }
    if (nlayouts < LAYOUTS) {
        i = nlayouts++;
    } else {
        i = layout_next;
        layout_next = (layout_next + 1) % LAYOUTS;
    }
    vtx_layout(s, vat, &layouts[i]);
    for (k = 0; k < 16; k++) {
        const VtxLayout* l = &layouts[i];
        bool on = (l->attrs >> k) & 1;

        layout_attribs[i].format[k] = on ? dev_attrib_format(l->out_size, l->attr[k].count, l->attr[k].type)
                                         : dev_attrib_format(0, 0, VTX_F32);
        layout_attribs[i].offset[k] = on ? l->attr[k].offset : 0;
    }
    if ((s->vcd_lo >> 8) & 1) {
        unsupported(U_TEX7_MTXIDX, "a texcoord 7 matrix index in the vertex");
    }
    return &layouts[i];
}

// Textures

// GX texture data is 32-byte aligned and sized, so it hashes by words.
static u32 tex_hash(const u8* p, u32 size)
{
    u32 h = 2166136261u, i, step;

    if (size <= TEX_HASH_FULL) {
        return hash_words(p, size, h);
    }
    h = hash_words(p, 256, h);
    h = hash_words(p + size - 256, 256, h);
    step = ((size - 512) / 64) & ~3u;
    for (i = 0; i < 64; i++) {
        h = hash_words(p + 256 + i * step, 8, h);
    }
    return h;
}

// Bumped each time cached textures are freed, so a texture loop that freed some starts again.
static u32 tex_frees;

static u32 tex_bucket_of(u32 addr)
{
    return ((addr >> 5) * 2654435761u) >> 22;
}

static void tex_link(TexEntry* e)
{
    u32 b = tex_bucket_of(e->addr);

    e->next = tex_bucket[b];
    tex_bucket[b] = (s16) (e - tex_cache);
}

static void tex_free(TexEntry* e)
{
    s16 idx = (s16) (e - tex_cache);
    s16* p = &tex_bucket[tex_bucket_of(e->addr)];

    while (*p != idx) {
        p = &tex_cache[*p].next;
    }
    *p = e->next;
    dev_local_free(e->mem);
    e->used = false;
}

// The bindings and lookups that may name a freed entry go; the textures they bound count as used in this frame.
static void tex_forget(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (bound[i] != NULL) {
            ((TexEntry*) bound[i])->frame = frame;
        }
    }
    memset(bound, 0, sizeof(bound));
    memset(tex_memo, 0, sizeof(tex_memo));
    textures_bound = false;
    memo.tex_ok = false;
    tex_frees++;
}

// The RSX may still sample e: bound now, or bound in this frame (finish() waits for the RSX, then counts on).
static bool tex_in_use(const TexEntry* e)
{
    int i;

    if (e->frame == frame) {
        return true;
    }
    for (i = 0; i < 8; i++) {
        if (bound[i] == e) {
            return true;
        }
    }
    return false;
}

// A unit's binding changes: its old texture counts as used in this frame.
static void tex_bind(int unit, const TexEntry* e)
{
    if (bound[unit] != NULL && bound[unit] != e) {
        ((TexEntry*) bound[unit])->frame = frame;
    }
    bound[unit] = e;
}

// Frees e, waiting for the RSX only when it may still sample e.
static void tex_evict(TexEntry* e)
{
    if (tex_in_use(e)) {
        dev_finish();
        ps3_prof.tex_waits++;
    }
    tex_free(e);
    tex_forget();
    ps3_prof.tex_evictions++;
}

static void free_textures(bool keep_efb)
{
    int i;

    dev_finish();
    for (i = 0; i < TEX_CACHE; i++) {
        if (tex_cache[i].used && !(keep_efb && tex_cache[i].efb)) {
            tex_free(&tex_cache[i]);
        }
    }
    tex_forget();
}

static void* tex_alloc(u32 size, u32* offset)
{
    void* mem = dev_local_alloc(128, size, offset);

    if (mem == NULL) {
        free_textures(true);
        mem = dev_local_alloc(128, size, offset);
    }
    return mem;
}

// A free entry, not linked; the least recently bound one goes when none is free.
static TexEntry* tex_slot(void)
{
    TexEntry* pick = NULL;
    int i;

    for (i = 0; i < TEX_CACHE; i++) {
        TexEntry* e = &tex_cache[i];

        if (!e->used) {
            return e;
        }
        // An EFB copy cannot be decoded again from memory: it goes last.
        if (pick == NULL || (pick->efb && !e->efb) || (pick->efb == e->efb && e->frame < pick->frame)) {
            pick = e;
        }
    }
    tex_evict(pick);
    return pick;
}

// False when the texture's data was written since it was hashed.
static bool tex_current(TexEntry* e)
{
    u32 now = ps3_mem_stamp();

    if (e->efb || now == e->stamp) {
        return true;
    }
    if (ps3_mem_written_since(e->addr, e->bytes, e->stamp)) {
        return false;
    }
    e->stamp = now;
    return true;
}

// The hash of n TMEM TLUT entries at byte off, kept until the next TLUT load.
static u32 tlut_hash_at(u32 off, u32 n)
{
    static struct {
        bool ok;
        u32 off, n, loads, hash;
    } memo_tlut[8];
    static int next;
    int i;

    for (i = 0; i < 8; i++) {
        if (memo_tlut[i].ok && memo_tlut[i].off == off && memo_tlut[i].n == n && memo_tlut[i].loads == tlut_loads) {
            return memo_tlut[i].hash;
        }
    }
    i = next;
    next = (next + 1) % 8;
    memo_tlut[i].ok = true;
    memo_tlut[i].off = off;
    memo_tlut[i].n = n;
    memo_tlut[i].loads = tlut_loads;
    memo_tlut[i].hash = hash_words(tmem_tlut + off, n * 2, 2166136261u);
    return memo_tlut[i].hash;
}

static u32 log2u(u32 v)
{
    u32 n = 0;

    while ((1u << n) < v) {
        n++;
    }
    return n;
}

// Decodes and uploads a texture; mipmapped ones are swizzled, the others linear.
static bool tex_upload(TexEntry* e, const u8* src, const u8* tlut, u32 tlut_entries)
{
    u32 w = e->width, h = e->height, level, size = 0, x, y;

    if (e->levels > 1) {
        u32 lw = log2u(w), lh = log2u(h), dst = 0;

        for (level = 0; level < e->levels; level++) {
            u32 sw = w >> level ? w >> level : 1, sh = h >> level ? h >> level : 1;
            u32 slw = lw > level ? lw - level : 0, slh = lh > level ? lh - level : 0;

            tex_decode(tex_temp, sw * 4, src, e->fmt, sw, sh, tlut, e->tlut_fmt, tlut_entries);
            for (y = 0; y < sh; y++) {
                for (x = 0; x < sw; x++) {
                    tex_swz[dst + tex_swizzle(x, y, slw, slh)] = tex_temp[y * sw + x];
                }
            }
            dst += sw * sh;
            src += tex_size(e->fmt, sw, sh);
        }
        size = dst * 4;
        e->pitch = 0;
        e->mem = tex_alloc(size, &e->offset);
        if (e->mem == NULL) {
            return false;
        }
        dev_write_local(e->mem, tex_swz, size);
    } else {
        // Linear textures keep a 64-byte aligned pitch.
        e->pitch = (w * 4 + 63) & ~63u;
        tex_decode(tex_temp, e->pitch, src, e->fmt, w, h, tlut, e->tlut_fmt, tlut_entries);
        e->mem = tex_alloc(e->pitch * h, &e->offset);
        if (e->mem == NULL) {
            return false;
        }
        dev_write_local(e->mem, tex_temp, e->pitch * h);
        size = e->pitch * h;
    }
    stats.texture_uploads++;
    ps3_prof.upload_bytes += size;
    return true;
}

static TexEntry* find_efb_texture(u32 addr, u32 w, u32 h)
{
    s16 i;

    for (i = tex_bucket[tex_bucket_of(addr)]; i >= 0; i = tex_cache[i].next) {
        TexEntry* e = &tex_cache[i];

        if (e->efb && e->addr == addr && e->width == w && e->height == h) {
            return e;
        }
    }
    return NULL;
}

// The entry for this data at addr; with check, only one whose data is unwritten since, else one of that hash.
static TexEntry* find_texture(u32 addr, u32 fmt, u32 w, u32 h, u32 levels, u32 tlut_fmt, u32 tlut_hash, bool check,
                              u32 hash)
{
    s16 i;

    for (i = tex_bucket[tex_bucket_of(addr)]; i >= 0; i = tex_cache[i].next) {
        TexEntry* t = &tex_cache[i];

        if (!t->efb && t->addr == addr && t->fmt == fmt && t->width == w && t->height == h && t->levels == levels &&
            t->tlut_hash == tlut_hash && t->tlut_fmt == tlut_fmt && (check ? tex_current(t) : t->hash == hash)) {
            return t;
        }
    }
    return NULL;
}

// Data rewritten in place (animated textures) leaves a version per content: past TEX_VERSIONS the oldest goes.
static void tex_limit_versions(u32 addr, u32 fmt, u32 w, u32 h, u32 levels, u32 tlut_fmt)
{
    TexEntry* oldest = NULL;
    int n = 0;
    s16 i;

    for (i = tex_bucket[tex_bucket_of(addr)]; i >= 0; i = tex_cache[i].next) {
        TexEntry* t = &tex_cache[i];

        if (!t->efb && t->addr == addr && t->fmt == fmt && t->width == w && t->height == h && t->levels == levels &&
            t->tlut_fmt == tlut_fmt)
        {
            n++;
            oldest = oldest == NULL || t->frame < oldest->frame ? t : oldest;
        }
    }
    if (n >= TEX_VERSIONS) {
        tex_evict(oldest);
    }
}

// The texture on GX map unit, from its BP registers; NULL when it cannot be read.
static TexEntry* tex_get(const GpState* s, int unit, DevTexture* d)
{
    static const u8 ids[5][8] = {
        { 0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3 }, { 0x84, 0x85, 0x86, 0x87, 0xA4, 0xA5, 0xA6, 0xA7 },
        { 0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB }, { 0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7 },
        { 0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB },
    };
    static const u8 wrap[4] = { DEV_WRAP_CLAMP, DEV_WRAP_REPEAT, DEV_WRAP_MIRROR, DEV_WRAP_CLAMP };
    u32 mode0 = s->bp[ids[0][unit]], mode1 = s->bp[ids[1][unit]], image0 = s->bp[ids[2][unit]];
    u32 addr = (s->bp[ids[3][unit]] & 0xFFFFFF) << 5, tlut_reg = s->bp[ids[4][unit]];
    u32 w = (image0 & 0x3FF) + 1, h = ((image0 >> 10) & 0x3FF) + 1, fmt = (image0 >> 20) & 0xF;
    u32 minf = (mode0 >> 5) & 7, maxlod = (mode1 >> 8) & 0xFF, levels = 1, hash = 0, tlut_hash = 0, tlut_fmt = 0;
    u32 tlut_entries = 0, stamp = 0, i;
    const u8* tlut = NULL;
    const u8* src;
    TexEntry* e;
    bool mip = (minf & 3) != 0 && maxlod > 0;

    e = find_efb_texture(addr, w, h);
    if (e == NULL) {
        u32 size, maxlevels;

        if (tex_size(fmt, w, h) == 0 || w > MAX_TEX_SIZE || h > MAX_TEX_SIZE) {
            unsupported(U_TEX_FORMAT, "a texture format");
            return NULL;
        }
        maxlevels = log2u(w > h ? w : h) + 1;
        if (mip && (w & (w - 1)) == 0 && (h & (h - 1)) == 0) {
            levels = maxlod / 16 + 1;
            levels = levels > maxlevels ? maxlevels : levels;
        }
        size = 0;
        for (i = 0; i < levels; i++) {
            size += tex_size(fmt, w >> i ? w >> i : 1, h >> i ? h >> i : 1);
        }
        src = OSPhysicalToCached(addr);
        if (src == NULL) {
            return NULL;
        }
        if (tex_is_ci(fmt)) {
            u32 off = (tlut_reg & 0x3FF) << 9;

            tlut_fmt = (tlut_reg >> 10) & 3;
            tlut_entries = fmt == TF_C4 ? 16 : fmt == TF_C8 ? 256 : 16384;
            if (off + tlut_entries * 2 > sizeof(tmem_tlut)) {
                tlut_entries = (sizeof(tmem_tlut) - off) / 2;
            }
            tlut = tmem_tlut + off;
            tlut_hash = tlut_hash_at(off, tlut_entries);
        }
        e = find_texture(addr, fmt, w, h, levels, tlut_fmt, tlut_hash, true, 0);
        if (e == NULL) {
            // Written since, or new: the data is hashed, and an entry with that hash is current again.
            stamp = ps3_mem_stamp();
            hash = tex_hash(src, size);
            e = find_texture(addr, fmt, w, h, levels, tlut_fmt, tlut_hash, false, hash);
            if (e != NULL) {
                e->stamp = stamp;
            }
            ps3_prof.tex_hashes++;
        }
        if (e == NULL) {
            tex_limit_versions(addr, fmt, w, h, levels, tlut_fmt);
            e = tex_slot();
            memset(e, 0, sizeof(*e));
            e->addr = addr;
            e->fmt = fmt;
            e->width = w;
            e->height = h;
            e->levels = levels;
            e->bytes = size;
            e->hash = hash;
            e->stamp = stamp;
            e->tlut_hash = tlut_hash;
            e->tlut_fmt = tlut_fmt;
            if (!tex_upload(e, src, tlut, tlut_entries)) {
                return NULL;
            }
            e->used = true;
            tex_link(e);
            stats.textures++;
        }
    }
    e->frame = frame;
    memset(d, 0, sizeof(*d));
    d->offset = e->offset;
    d->pitch = e->pitch;
    d->width = (u16) e->width;
    d->height = (u16) e->height;
    d->levels = (u8) e->levels;
    d->swizzled = e->levels > 1;
    d->wrap_s = wrap[mode0 & 3];
    d->wrap_t = wrap[(mode0 >> 2) & 3];
    d->mag_filter = (mode0 >> 4) & 1 ? DEV_FILTER_LINEAR : DEV_FILTER_NEAREST;
    if (e->levels > 1) {
        static const u8 mins[8] = { DEV_FILTER_NEAREST, DEV_FILTER_NEAREST_MIP_NEAREST, DEV_FILTER_NEAREST_MIP_LINEAR,
                                    DEV_FILTER_NEAREST, DEV_FILTER_LINEAR, DEV_FILTER_LINEAR_MIP_NEAREST,
                                    DEV_FILTER_LINEAR_MIP_LINEAR, DEV_FILTER_LINEAR };
        d->min_filter = mins[minf];
    } else {
        d->min_filter = (minf & 4) ? DEV_FILTER_LINEAR : DEV_FILTER_NEAREST;
    }
    // GX LODs are in 1/16, the bias in 1/32: both to 4.8.
    d->min_lod = (u16) ((mode1 & 0xFF) << 4);
    d->max_lod = (u16) ((e->levels - 1) << 8);
    if (((mode1 >> 8) & 0xFF) << 4 < d->max_lod) {
        d->max_lod = (u16) (((mode1 >> 8) & 0xFF) << 4);
    }
    d->lod_bias = (s16) ((s8) ((mode0 >> 9) & 0xFF) * 8);
    return e;
}

#ifdef GX_TCACHE_CHECK
// Check build (host): textures bind at each draw, and each one's data must still have the hash it was cached with.
static void tex_check(const TexEntry* e)
{
    static u32 checks, errors, logged, last;
    const u8* src = OSPhysicalToCached(e->addr);

    if (e->efb || src == NULL) {
        return;
    }
    if (tex_hash(src, e->bytes) != e->hash && ++errors != 0 && e->addr != last && logged++ < 200) {
        last = e->addr;
        ps3_log("tcache check: texture %08x (%ux%u format %u) drawn with changed data\n", e->addr, e->width,
                e->height, e->fmt);
    }
    if (++checks % 1000000 == 0) {
        ps3_log("tcache check: %u textures drawn, %u with changed data\n", checks, errors);
    }
}
#endif

// A/B: a white 16x16 texture with d's sampling, in place of a fighter's shadow copy; made at the first use.
static const DevTexture* white_for(const DevTexture* d)
{
    static DevTexture w;
    static u32 texels[16 * 16], off;
    static bool made;

    if (!made) {
        void* mem = dev_local_alloc(128, sizeof(texels), &off);

        if (mem == NULL) {
            return d;
        }
        memset(texels, 0xFF, sizeof(texels));
        dev_write_local(mem, texels, sizeof(texels));
        made = true;
    }
    w = *d;
    w.offset = off;
    w.pitch = 64;
    w.width = w.height = 16;
    w.levels = 1;
    w.swizzled = 0;
    return &w;
}

// GX map unit's BP register k: mode 0, mode 1, image 0, image 3, TLUT.
static u32 tex_reg(const GpState* s, int unit, int k)
{
    static const u8 base[2] = { 0x80, 0xA0 };
    static const u8 off[5] = { 0x00, 0x04, 0x08, 0x14, 0x18 };

    return s->bp[base[unit >> 2] + off[k] + (unit & 3)];
}

#ifdef GX_TCACHE_CHECK
// dev_texture calls by bind_textures.
static u32 tex_binds;
#endif

// Returns the units the config samples.
static u32 bind_textures(const GpState* s, const TevConfig* cfg)
{
    bool used[8] = { false };
    u32 frees = tex_frees, restarts = 0, mask = 0;
    int i, k;

    for (i = 0; i < cfg->nstages; i++) {
        if (cfg->st[i].tex >= 0) {
            used[cfg->st[i].tex] = true;
            mask |= 1u << cfg->st[i].tex;
        }
    }
    for (i = 0; i < 8; i++) {
        u32 regs[5];
        bool same;

        if (!used[i]) {
            continue;
        }
        for (k = 0; k < 5; k++) {
            regs[k] = tex_reg(s, i, k);
        }
        same = tex_memo[i].e != NULL && tex_memo[i].tlut_loads == tlut_loads &&
               memcmp(tex_memo[i].regs, regs, sizeof(regs)) == 0 && tex_current(tex_memo[i].e);
        if (!same) {
            tex_memo[i].e = tex_get(s, i, &tex_memo[i].d);
            if (tex_memo[i].e == NULL) {
                // The unit is off: it samples black.
                ps3_prof.tex_failures++;
            }
            memcpy(tex_memo[i].regs, regs, sizeof(regs));
            tex_memo[i].tlut_loads = tlut_loads;
            if (tex_frees != frees && restarts++ < 4) {
                // The lookup freed textures, maybe ones bound for earlier units: all of them again.
                frees = tex_frees;
                i = -1;
                continue;
            }
        }
        if (tex_memo[i].e != NULL) {
            tex_memo[i].e->frame = frame;
#ifdef GX_TCACHE_CHECK
            tex_check(tex_memo[i].e);
#endif
        }
        if (!same || !textures_bound || bound[i] != tex_memo[i].e) {
            const TexEntry* e = tex_memo[i].e;
            const DevTexture* d = e != NULL ? &tex_memo[i].d : NULL;

            if (draw_ab & 256) {
                dev_barrier();
            }
            // A/B: the fighters' shadows (lbshadow.c's 256x256 EFB copies) sample white.
            if ((draw_ab & 262144) && e != NULL && e->efb && e->width == 256 && e->height == 256) {
                d = white_for(d);
            }
#ifdef GX_TCACHE_CHECK
            tex_binds++;
#endif
            dev_texture(i, d);
            tex_bind(i, e);
        }
    }
    // Past the last restart, a free emptied bound[]: the units count as unbound.
    textures_bound = tex_frees == frees;
    return mask;
}

// The used units' texture registers as at the last bind_textures.
static bool tex_regs_same(const GpState* s)
{
    u32 used = memo.tex_used;
    int i, k;

    for (i = 0; used != 0; i++, used >>= 1) {
        if (used & 1) {
            for (k = 0; k < 5; k++) {
                if (memo.tex_regs[i][k] != tex_reg(s, i, k)) {
                    return false;
                }
            }
        }
    }
    return true;
}

// Draws

static u32 prim_type(int prim)
{
    static const u8 types[8] = { DEV_PRIM_QUADS, DEV_PRIM_QUADS, DEV_PRIM_TRIANGLES, DEV_PRIM_TRIANGLE_STRIP,
                                 DEV_PRIM_TRIANGLE_FAN, DEV_PRIM_LINES, DEV_PRIM_LINE_STRIP, DEV_PRIM_POINTS };
    return types[prim & 7];
}

// The RSX family of GX primitive prim: batches draw triangles, lines or points.
static u32 prim_family(int prim)
{
    return prim <= GP_TRIANGLE_FAN ? DEV_PRIM_TRIANGLES : prim == GP_POINTS ? DEV_PRIM_POINTS : DEV_PRIM_LINES;
}

// Indices of count vertices of GX primitive prim in its family.
static u32 index_count(int prim, u32 count)
{
    switch (prim) {
    case GP_QUADS:
    case GP_QUADS2:
        return count / 4 * 6;
    case GP_TRIANGLES:
        return count / 3 * 3;
    case GP_TRIANGLE_STRIP:
    case GP_TRIANGLE_FAN:
        return count >= 3 ? 3 * (count - 2) : 0;
    case GP_LINES:
        return count / 2 * 2;
    case GP_LINE_STRIP:
        return count >= 2 ? 2 * (count - 1) : 0;
    default:
        return count;
    }
}

// The triangles as the RSX splits quads, strips and fans: each keeps the facing it had.
static void put_indices(u16* d, int prim, u32 b, u32 count)
{
    u32 i, n;

    switch (prim) {
    case GP_QUADS:
    case GP_QUADS2:
        for (i = 0; i + 3 < count; i += 4, d += 6) {
            d[0] = (u16) (b + i);
            d[1] = (u16) (b + i + 1);
            d[2] = (u16) (b + i + 2);
            d[3] = (u16) (b + i);
            d[4] = (u16) (b + i + 2);
            d[5] = (u16) (b + i + 3);
        }
        break;
    case GP_TRIANGLE_STRIP:
        // Odd triangles swap their first two vertices.
        for (i = 0; i + 2 < count; i++, d += 3) {
            d[0] = (u16) (b + i + (i & 1));
            d[1] = (u16) (b + i + 1 - (i & 1));
            d[2] = (u16) (b + i + 2);
        }
        break;
    case GP_TRIANGLE_FAN:
        for (i = 1; i + 1 < count; i++, d += 3) {
            d[0] = (u16) b;
            d[1] = (u16) (b + i);
            d[2] = (u16) (b + i + 1);
        }
        break;
    case GP_LINE_STRIP:
        for (i = 0; i + 1 < count; i++, d += 2) {
            d[0] = (u16) (b + i);
            d[1] = (u16) (b + i + 1);
        }
        break;
    default:
        // Triangles, lines and points in order, whole primitives only.
        n = index_count(prim, count);
        for (i = 0; i < n; i++) {
            d[i] = (u16) (b + i);
        }
        break;
    }
}

// The layout of the RSX's vertex bindings; NULL after a copy or present pass bound its own.
static const VtxLayout* bound_layout;

#ifdef GX_ATTRIB_CHECK
// Check build (host): after each bind the device's registers must hold what the old per-slot dev_attrib calls set.
static void attrib_check(const VtxLayout* l, u32 io)
{
    static u32 checks, errors;
    u32 format[16], offset[16], i;

    dev_debug_attribs(format, offset);
    checks++;
    for (i = 0; i < 16; i++) {
        bool on = (l->attrs >> i) & 1;
        u32 a = on ? io + l->attr[i].offset : 0, f = VTX_F32, o;

        // As librsx's rsxBindVertexArrayAttrib encodes dev_attrib's arguments.
        if (on) {
            f = (u32) (u8) l->out_size << 8 | (u32) (u8) l->attr[i].count << 4 | (u8) l->attr[i].type;
        }
        o = (a & ~DEV_LOCAL) | ((a & DEV_LOCAL) ? 0 : 1u << 31);
        if (format[i] != f || offset[i] != o) {
            if (errors++ < 20) {
                ps3_log("attrib check: bind %u slot %u: %08x %08x against %08x %08x\n", checks, i, format[i],
                        offset[i], f, o);
            }
            break;
        }
    }
    if (checks % 100000 == 0) {
        ps3_log("attrib check: %u binds, %u differ\n", checks, errors);
    }
}
#endif

static void bind_attribs(const VtxLayout* l, u32 io)
{
    u32 i;

    // A/B: the RSX finishes its draws before the vertex format changes (offsets change at nearly every draw).
    if ((ps3_diag.gx_ab & 32768) && l != bound_layout) {
        dev_barrier();
    }
    bound_layout = l;
    if (!(ps3_diag.gx_ab & 67108864)) {
        dev_attribs(layout_attribs[l - layouts].format, layout_attribs[l - layouts].offset, io, l->attrs);
    } else {
        // A/B: a librsx call per slot.
        for (i = 0; i < 16; i++) {
            if ((l->attrs >> i) & 1) {
                dev_attrib((int) i, io + l->attr[i].offset, l->out_size, l->attr[i].count, l->attr[i].type);
            } else {
                dev_attrib((int) i, 0, 0, 0, VTX_F32);
            }
        }
    }
#ifdef GX_ATTRIB_CHECK
    attrib_check(l, io);
#endif
}

// A/B after an RSX draw (diag.h): the PPE waits for it (8); the RSX waits for its own backend after every draw (16),
// after cached list draws (4096) or after ring draws (8192); or the commands go to the RSX at once (32).
static void after_rsx_draw(bool cached)
{
    unsigned ab = ps3_diag.gx_ab;

    if (ab & 8) {
        dev_finish();
    } else if ((ab & 16) || (ab & (cached ? 4096u : 8192u))) {
        dev_barrier();
    } else if (ab & 32) {
        dev_flush();
    }
}

// RSX draws since the last display copy, and the display copies so far (GX frames).
static u32 frame_draws, gx_frame;

// A/B: each RSX draw of a frame lies 2 depth steps nearer than the one before, so a later draw wins a near tie, as
// GX's equal depths with "less or equal" do. Called before each RSX draw of the GX draws.
static void before_rsx_draw(void)
{
    static bool on;

    if (ps3_diag.gx_ab & 1048576) {
        dev_depth_bias(-2.0f * (float) frame_draws);
        on = true;
    } else if (on) {
        dev_depth_bias(0.0f);
        on = false;
    }
    frame_draws++;
}

// Sends the open batch. Each backend call that sends other RSX commands, or waits for the RSX, calls it first.
static void batch_flush(void)
{
    u32 at;

    if (!batch.open) {
        return;
    }
    batch.open = false;
    at = (batch.verts * batch.l->out_size + 15) & ~15u;
    if (batch.nidx == 0) {
        dev_ring_commit(0);
        return;
    }
    memcpy(batch.base + at, batch_idx, batch.nidx * 2);
    dev_ring_commit(at + batch.nidx * 2);
    bind_attribs(batch.l, batch.io);
    before_rsx_draw();
    dev_draw_indexed(batch.family, batch.io + at, batch.nidx);
    stats.rsx_draws++;
    after_rsx_draw(false);
}

static bool batch_open(const GpState* s, const VtxLayout* l, int vat, u32 count, u32 family, u32 nidx)
{
    batch.base = dev_ring_reserve(count * l->out_size + 16 + nidx * 2, &batch.avail, &batch.io);
    if (batch.base == NULL) {
        return false;
    }
    batch.open = true;
    batch.family = family;
    batch.writes = s->writes;
    batch.vat = vat;
    batch.l = l;
    batch.verts = 0;
    batch.nidx = 0;
    return true;
}

// False when the draw does not fit in the open batch.
static bool batch_add(const GpState* s, int prim, u32 count, const u8* data, u32 nidx)
{
    // The SPU writes whole 16-byte lines: each draw's vertices start on one, the vertices between go unused.
    u32 first = vx_align(batch.verts, batch.l->out_size), verts = first + count, total = batch.nidx + nidx;
    u8* out = batch.base + first * batch.l->out_size;

    if (verts > BATCH_VERTICES || total > BATCH_INDICES ||
        ((verts * batch.l->out_size + 15) & ~15u) + total * 2 > batch.avail) {
        return false;
    }
    if (!vx_submit(batch.l, s, data, count, out) && !vtx_convert(batch.l, s, data, count, out)) {
        stats.dropped_draws++;
        return true;
    }
    put_indices(batch_idx + batch.nidx, prim, first, count);
    batch.verts = verts;
    batch.nidx = total;
    return true;
}

// A draw too large for a batch, as it is.
static void draw_direct(const GpState* s, const VtxLayout* l, int prim, u32 count, const u8* data)
{
    u32 io;
    u8* out = dev_ring_alloc(count * l->out_size, &io);

    if (out == NULL || !vtx_convert(l, s, data, count, out)) {
        stats.dropped_draws++;
        return;
    }
    bind_attribs(l, io);
    before_rsx_draw();
    dev_draw(prim_type(prim), count);
    stats.rsx_draws++;
    after_rsx_draw(false);
}


// The layout of VAT vat under the current VCD, kept while VCD and VAT stay.
static VtxLayout* current_layout(const GpState* s, int vat)
{
    if (!memo.layout_ok || memo.layout_cp != s->cp_gen || memo.layout_vat != vat) {
        memo.layout = get_layout(s, vat);
        memo.layout_cp = s->cp_gen;
        memo.layout_vat = vat;
        memo.layout_ok = true;
        memo.consts_ok = false;
    }
    return memo.layout;
}

// The RSX state of a draw that does not join the open batch: programs, raster state, constants, textures. Its
// layout, or NULL when the draw is not drawn (the caller counts it as dropped).
// The texcoord offsets of a point or line draw, for the TEV config: TCO_* << 16 | the divisor << 8 | the texcoords
// that take it (SU_SSIZE point and line offset bits). 0 when none.
static u32 tex_offsets(const GpState* s, int prim)
{
    static const u8 divisor[8] = { 0, 16, 8, 4, 2, 1, 1, 1 };
    u32 lp = s->bp[0x22], kind, d, mask = 0, bit, i;

    if (prim == GP_POINTS) {
        kind = TCO_POINT;
        d = divisor[(lp >> 19) & 7];
        bit = 19;
    } else if (prim == GP_LINES || prim == GP_LINE_STRIP) {
        kind = TCO_LINE;
        d = divisor[(lp >> 16) & 7];
        bit = 18;
    } else {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        mask |= ((s->bp[0x30 + 2 * i] >> bit) & 1) << i;
    }
    return d == 0 || mask == 0 ? 0 : kind << 16 | d << 8 | mask;
}

static const VtxLayout* draw_setup(GpState* s, int prim, int vat, u32 count, uint64_t* t)
{
    VtxLayout* l;
    const FpEntry* f;
    VpEntry* v;
    XfKey key;
    u32 numtexgens = s->xf_regs[0x3F] & 0xF, stamp, tex_ab, tco;
    bool tev_hit, tex_hit, reused = true, nospec, olddepth;
    int i, k;

    draw_ab = ps3_diag.gx_ab;

    l = current_layout(s, vat);
    if (!(l->attrs & (1u << VA_POS)) || (((s->bp[0x00] >> 14) & 3) == 3 && prim <= 4)) {
        return NULL;
    }
    numtexgens = numtexgens > 8 ? 8 : numtexgens;
    ps3_diag.gp_site = "draw: TEV";
    tco = tex_offsets(s, prim);
    tev_hit = memo.tev_ok && memo.tev_gen == s->tev_gen && memo.tev_tco == tco;
    if (!tev_hit) {
        reused = false;
        tev_setup(s, numtexgens, tco);
    }
#ifdef GX_TEV_CHECK
    if (memo.tev_ok) {
        tev_check(s, numtexgens, tco);
    }
#endif
    f = memo.f;
    ps3_prof_lap(PROF_DRAW_TEV, t);
    ps3_diag.gp_site = "draw: XF";
    nospec = (draw_ab & 2048) != 0;
    olddepth = (draw_ab & 4194304) != 0;
    if (!memo.vp_ok || memo.vp_xf != s->xf_gen || memo.vp_attrs != l->attrs || memo.vp_pos3 != l->pos3 ||
        memo.vp_tev != memo.tev_mask || memo.vp_nospec != nospec || memo.vp_olddepth != olddepth) {
        reused = false;
        xf_make_key(s, l->attrs, l->pos3, memo.tev_mask, &key);
        key.old_depth = olddepth;
        if (nospec) {
            // A/B: specular channels (attenuation on, spot off) light nothing and give their ambient colour.
            for (i = 0; i < 4; i++) {
                if (((key.chan[i] >> 9) & 3) == 1) {
                    key.chan[i] &= ~0x783Cu;
                }
            }
        }
        memo.v = vp_get(&key);
        memo.vp_ok = memo.v != NULL;
        memo.vp_xf = s->xf_gen;
        memo.vp_attrs = l->attrs;
        memo.vp_pos3 = l->pos3;
        memo.vp_tev = memo.tev_mask;
        memo.vp_nospec = nospec;
        memo.vp_olddepth = olddepth;
    }
    v = memo.v;
    ps3_prof_lap(PROF_DRAW_XF, t);
    ps3_diag.gp_site = "draw: state";
    if (f == NULL || v == NULL) {
        return NULL;
    }
    // The viewport's depth follows the program's depth output (apply_state). Other BP or XF writes keep the state.
    if (!memo.state_ok || memo.state_olddepth != olddepth ||
        ((memo.state_bp != s->bp_gen || memo.state_xf != s->xf_gen) && !state_same(s))) {
        reused = false;
        memo.state_pass = apply_state(s);
        state_save(s);
        memo.state_olddepth = olddepth;
        memo.state_ok = true;
    }
#ifdef GX_STATE_CHECK
    else if (memo.state_bp != s->bp_gen || memo.state_xf != s->xf_gen) {
        state_check(s);
    }
#endif
    memo.state_bp = s->bp_gen;
    memo.state_xf = s->xf_gen;
    if (!memo.state_pass) {
        return NULL;
    }
    if (reused) {
        ps3_prof.state_reuses++;
    }
    ps3_prof_lap(PROF_DRAW_STATE, t);
    ps3_diag.gp_site = "draw: constants";
    update_constants(s, l, memo.tev, memo.tev_mask);
    ps3_prof_lap(PROF_DRAW_CONST, t);
    ps3_diag.gp_site = "draw: programs and textures";
    use_vp(v);
    use_fp(f);
    dev_point_sprite(prim == GP_POINTS && (tco >> 16) == TCO_POINT);
    // The textures are checked again after a write to their registers, a TLUT load or a memory write
    // (bind_textures).
    stamp = ps3_mem_stamp();
    tex_ab = draw_ab & (256 | 262144);
    tex_hit = memo.tex_ok && textures_bound && memo.tex_stamp == stamp && memo.tex_tluts == tlut_loads &&
              memo.tex_ab == tex_ab && (memo.tex_bp == s->bp_gen || tex_regs_same(s));
#ifdef GX_TCACHE_CHECK
    {
        // Check build: the textures bind at each draw; where the memo keeps them, nothing may change.
        static u32 kept, errors;
        const TexEntry* was[8];
        u32 binds = tex_binds;

        memcpy(was, bound, sizeof(was));
        memo.tex_used = bind_textures(s, &f->cfg);
        if (tex_hit && (tex_binds != binds || memcmp(was, bound, sizeof(was)) != 0) && errors++ < 20) {
            ps3_log("tcache check: kept bindings %u changed\n", kept);
        }
        if (tex_hit && ++kept % 100000 == 0) {
            ps3_log("tcache check: %u kept bindings, %u changed\n", kept, errors);
        }
        tex_hit = false;
    }
#endif
    if (!tex_hit) {
#ifndef GX_TCACHE_CHECK
        memo.tex_used = bind_textures(s, &f->cfg);
#endif
        for (i = 0; i < 8; i++) {
            if ((memo.tex_used >> i) & 1) {
                for (k = 0; k < 5; k++) {
                    memo.tex_regs[i][k] = tex_reg(s, i, k);
                }
            }
        }
        memo.tex_stamp = stamp;
        memo.tex_tluts = tlut_loads;
        memo.tex_ab = tex_ab;
        // A free after bind_textures' last pass cleared bound[]: bind again at the next draw.
        memo.tex_ok = textures_bound;
    }
    memo.tex_bp = s->bp_gen;
    ps3_prof_lap(PROF_DRAW_TEX, t);
    if (trace_left != 0) {
        trace_left--;
        ps3_log("backend: draw %u prim %d vat %d count %u attrs %#x size %u vp %u fp %u\n", stats.draws, prim,
                vat, count, l->attrs, l->out_size, v->n, f->offset);
    }
    return l;
}

static void draw(GpState* s, int prim, int vat, u32 count, const u8* data)
{
    const VtxLayout* l;
    u32 family = prim_family(prim), nidx = index_count(prim, count);
    uint64_t t0 = ps3_prof_now(), t = t0;

    stats.draws++;
    stats.vertices += count;
    if (nidx == 0) {
        return;
    }
    ps3_diag.gp_draw = stats.draws;
    if (batch.open && batch.writes == s->writes && batch.vat == vat && batch.family == family &&
        !(ps3_diag.gx_ab & 1)) {
        ps3_diag.gp_site = "draw: batch";
        ps3_prof.batched++;
        if (!batch_add(s, prim, count, data, nidx)) {
            // The reservation is full: the batch so far goes, the state stays for the next one.
            const VtxLayout* bl = batch.l;

            batch_flush();
            if (!batch_open(s, bl, vat, count, family, nidx) || !batch_add(s, prim, count, data, nidx)) {
                stats.dropped_draws++;
            }
        }
        ps3_prof_lap(PROF_DRAW_VTX, &t);
        ps3_prof.ticks[PROF_DRAW] += t - t0;
        return;
    }
    batch_flush();
    l = draw_setup(s, prim, vat, count, &t);
    if (l == NULL) {
        stats.dropped_draws++;
        return;
    }
    ps3_diag.gp_site = "draw: vertices";
    if (count > BATCH_VERTICES || nidx > BATCH_INDICES || (ps3_diag.gx_ab & 1)) {
        draw_direct(s, l, prim, count, data);
    } else if (!batch_open(s, l, vat, count, family, nidx) || !batch_add(s, prim, count, data, nidx)) {
        stats.dropped_draws++;
    }
    ps3_prof_lap(PROF_DRAW_VTX, &t);
    ps3_prof.ticks[PROF_DRAW] += t - t0;
}

// Display list cache

// The vertices and indices of a display list made only of draws, converted once into local memory and drawn from
// there while the list, its arrays and the state they were read with stay. The memory is a ring: the oldest go.
#define VC_HEAP (16u << 20)
#define VC_ENTRIES 4096
#define VC_BUCKETS 4096
// Fountain of Dreams' star field is one list of 1,852 point draws.
#define VC_MAX_DRAWS 4096
// The largest entry: vertices, then indices.
#define VC_STAGE (1u << 20)

typedef struct VcEntry {
    u32 paddr, size;
    // used: the slot is taken; linked: in its hash chain. A stale cached entry keeps its slot until its heap
    // space goes.
    bool used, linked;
    // false: a list run as commands (other commands, several VATs or families, too large)
    bool cached;
    u8 vat, prim;
    u32 family;
    // The arrays the vertices were converted with (the rest of that state is in its VcKey).
    u16 arrays;
    u32 base[VTX_ARRAYS];
    u8 stride[VTX_ARRAYS];
    // The physical bytes read from each array, and the ps3_mem_stamp they were read at.
    u32 read_at[VTX_ARRAYS], read_len[VTX_ARRAYS];
    u32 stamp;
    // Local memory: off from the heap start, the indices at off + idx_at.
    u32 off, bytes, idx_at;
    u32 nidx, ndraws, nverts;
    u32 frame; // last drawn in this frame
    // The flicker detector's GX frame and draw count of this list in it.
    u32 watch_frame;
    u16 watch_occ;
} VcEntry;

static VcEntry vc_entries[VC_ENTRIES];
// What vc_find compares of each entry, apart, so a chain walk reads a few lines. The VAT words and default matrix
// indices only as far as the conversion read them (vc_masks).
typedef struct VcKey {
    u32 paddr, size, vcd_lo, vcd_hi;
    u32 vat_a, vat_b, vat_c, mtx_a, mtx_b;
    s32 next; // hash chain
    bool cached;
    u8 vat;
} VcKey;
static VcKey vc_keys[VC_ENTRIES];
static s32 vc_bucket[VC_BUCKETS];
static s32 vc_free[VC_ENTRIES];
static u32 vc_nfree;
// The entries in the heap, oldest first.
static s32 vc_order[VC_ENTRIES];
static u32 vc_order_first, vc_order_count;
static void* vc_mem;
static u32 vc_mem_off, vc_head;
static u8* vc_stage;
static GpListDraw vc_draws[VC_MAX_DRAWS];

static u32 vc_hash(u32 paddr, u32 size)
{
    return ((paddr >> 5) ^ (size * 2654435761u)) % VC_BUCKETS;
}

static void vc_reset(void)
{
    u32 i;

    for (i = 0; i < VC_BUCKETS; i++) {
        vc_bucket[i] = -1;
    }
    for (i = 0; i < VC_ENTRIES; i++) {
        vc_entries[i].used = false;
        vc_free[i] = (s32) (VC_ENTRIES - 1 - i);
    }
    vc_nfree = VC_ENTRIES;
    vc_order_first = vc_order_count = 0;
    vc_head = 0;
}

static void vc_unlink(VcEntry* e)
{
    s32 idx = (s32) (e - vc_entries);
    s32* p = &vc_bucket[vc_hash(e->paddr, e->size)];

    while (*p != idx) {
        p = &vc_keys[*p].next;
    }
    *p = vc_keys[idx].next;
    e->linked = false;
}

static void vc_release(VcEntry* e)
{
    if (e->linked) {
        vc_unlink(e);
    }
    e->used = false;
    vc_free[vc_nfree++] = (s32) (e - vc_entries);
}

// A list that no longer matches its memory: a cached one keeps its heap space until vc_evict_oldest.
static void vc_drop(VcEntry* e)
{
    if (e->cached) {
        vc_unlink(e);
    } else {
        vc_release(e);
    }
}

// The oldest entry in the heap goes. True when the RSX may still read it.
static bool vc_evict_oldest(void)
{
    VcEntry* e = &vc_entries[vc_order[vc_order_first]];
    bool busy = e->frame == frame;

    vc_order_first = (vc_order_first + 1) % VC_ENTRIES;
    vc_order_count--;
    vc_release(e);
    ps3_prof.list_evictions++;
    return busy;
}

// A free slot for a list, linked into its chain.
static VcEntry* vc_new(u32 paddr, u32 size)
{
    u32 h = vc_hash(paddr, size), i;
    VcEntry* e;
    VcKey* k;

    if (vc_nfree == 0) {
        // The lists run as commands go first: they are found again with one parse each.
        for (i = 0; i < VC_ENTRIES; i++) {
            if (vc_entries[i].used && !vc_entries[i].cached) {
                vc_release(&vc_entries[i]);
            }
        }
    }
    if (vc_nfree == 0 && vc_evict_oldest()) {
        // Its heap space leaves the order, so no later allocation waits for it.
        dev_finish();
    }
    e = &vc_entries[vc_free[--vc_nfree]];
    k = &vc_keys[e - vc_entries];
    memset(e, 0, sizeof(*e));
    memset(k, 0, sizeof(*k));
    e->paddr = k->paddr = paddr;
    e->size = k->size = size;
    e->used = true;
    e->linked = true;
    k->next = vc_bucket[h];
    vc_bucket[h] = (s32) (e - vc_entries);
    return e;
}

// size bytes of heap at *off, 16-byte aligned; the entries there go.
static bool vc_alloc(u32 size, u32* off)
{
    u32 at = vc_head;
    bool busy = false;

    if (size > VC_HEAP) {
        return false;
    }
    if (at + size > VC_HEAP) {
        // The tail is too short: its entries, the oldest, go, and the heap starts again at 0.
        while (vc_order_count != 0 && vc_entries[vc_order[vc_order_first]].off >= at) {
            busy |= vc_evict_oldest();
        }
        at = 0;
    }
    // The oldest entry is the first one at or after at; there are none ahead before the first wrap.
    while (vc_order_count != 0) {
        const VcEntry* o = &vc_entries[vc_order[vc_order_first]];

        if (o->off < at || o->off >= at + size) {
            break;
        }
        busy |= vc_evict_oldest();
    }
    if (busy) {
        dev_finish();
    }
    *off = at;
    vc_head = (at + size + 15) & ~15u;
    return true;
}

// The VAT A, B and C bits and the default matrix indices (XF 0x18, 0x19) that converting vertices under this VCD
// reads: the counts and formats of the attributes present. Fractions only scale, in the constants (vtx.c dq).
static const u32* vc_masks(u32 lo, u32 hi)
{
    static const u8 word[8] = { 0, 1, 1, 1, 1, 2, 2, 2 };
    static const u8 shift[8] = { 21, 0, 9, 18, 27, 5, 14, 23 };
    static u32 last_lo, last_hi, m[5];
    static bool ok;
    u32 i;

    if (ok && lo == last_lo && hi == last_hi) {
        return m;
    }
    ok = true;
    last_lo = lo;
    last_hi = hi;
    memset(m, 0, sizeof(m));
    m[0] |= ((lo >> 9) & 3) ? 0xFu : 0;
    // Normal: NBT and format; NBT3 only with indices.
    m[0] |= ((lo >> 11) & 3) ? 0x1E00u : 0;
    m[0] |= ((lo >> 11) & 3) >= 2 ? 0x80000000u : 0;
    m[0] |= ((lo >> 13) & 3) ? 0x1C000u : 0;
    m[0] |= ((lo >> 15) & 3) ? 0x1C0000u : 0;
    for (i = 0; i < 8; i++) {
        if ((hi >> (2 * i)) & 3) {
            m[word[i]] |= 0xFu << shift[i];
        }
    }
    // As vtx_layout: MIDX_A (position, texcoords 0-2) goes out when any index is in the vertex, MIDX_B (texcoords
    // 3-6) when one of its own is; a missing index takes its default.
    for (i = 0; i < 8; i++) {
        if (!((lo >> i) & 1) && (lo & (i < 4 ? 0xFFu : 0xF0u)) != 0) {
            if (i < 5) {
                m[3] |= 0x3Fu << (6 * i);
            } else {
                m[4] |= 0x3Fu << (6 * (i - 5));
            }
        }
    }
    return m;
}

// The entry for the list under the current state: its VCD, the VAT, arrays and default matrix indices it read.
static VcEntry* vc_find(const GpState* s, u32 paddr, u32 size)
{
    const u32* m = vc_masks(s->vcd_lo, s->vcd_hi);
    s32 i;

    for (i = vc_bucket[vc_hash(paddr, size)]; i >= 0; i = vc_keys[i].next) {
        const VcKey* c = &vc_keys[i];
        VcEntry* e;
        u32 k;

        if (c->paddr != paddr || c->size != size || c->vcd_lo != s->vcd_lo || c->vcd_hi != s->vcd_hi) {
            continue;
        }
        e = &vc_entries[i];
        if (!c->cached) {
            return e;
        }
        if (c->vat_a != (s->vat_a[c->vat] & m[0]) || c->vat_b != (s->vat_b[c->vat] & m[1]) ||
            c->vat_c != (s->vat_c[c->vat] & m[2]) || c->mtx_a != (s->xf_regs[0x18] & m[3]) ||
            c->mtx_b != (s->xf_regs[0x19] & m[4])) {
            continue;
        }
        for (k = 0; k < VTX_ARRAYS; k++) {
            if (((e->arrays >> k) & 1) && (e->base[k] != s->array_base[k] || e->stride[k] != s->array_stride[k])) {
                break;
            }
        }
        if (k == VTX_ARRAYS) {
            return e;
        }
    }
    return NULL;
}

// False when the list or the array bytes it read were written since it was converted.
static bool vc_current(VcEntry* e)
{
    u32 now = ps3_mem_stamp(), k;

    if (now == e->stamp) {
        return true;
    }
    if (ps3_mem_written_since(e->paddr, e->size, e->stamp)) {
        return false;
    }
    for (k = 0; k < VTX_ARRAYS; k++) {
        if (((e->arrays >> k) & 1) && ps3_mem_written_since(e->read_at[k], e->read_len[k], e->stamp)) {
            return false;
        }
    }
    e->stamp = now;
    return true;
}

// Converts the n list draws in vc_draws into vc_stage: the vertices, then at *idx_at the indices. Their sizes,
// or false when an array maps to nothing.
static bool vc_convert(GpState* s, const VtxLayout* l, u32 n, u32* nverts, u32* nidx, u32* idx_at, u32* bytes)
{
    u32 v = 0, x = 0, i, at;

    for (i = 0; i < n; i++) {
        v += vc_draws[i].count;
        x += index_count(vc_draws[i].prim, vc_draws[i].count);
    }
    at = (v * l->out_size + 15) & ~15u;
    *nverts = v;
    *nidx = x;
    *idx_at = at;
    *bytes = at + ((x * 2 + 15) & ~15u);
    if (v > BATCH_VERTICES || *bytes > VC_STAGE) {
        return false;
    }
    // The padding too, so a check build compares equal bytes.
    memset(vc_stage, 0, *bytes);
    v = 0;
    x = 0;
    for (i = 0; i < n; i++) {
        const GpListDraw* d = &vc_draws[i];

        if (!vtx_convert(l, s, d->data, d->count, vc_stage + v * l->out_size)) {
            return false;
        }
        put_indices((u16*) (vc_stage + at) + x, d->prim, v, d->count);
        v += d->count;
        x += index_count(d->prim, d->count);
    }
    return true;
}

// A new entry for the list: cached when it holds only draws of one VAT and family that fit; NULL when its arrays
// map to nothing or no slot is free.
static VcEntry* vc_build(GpState* s, u32 paddr, const u8* list, u32 size)
{
    u32 n, i, k, nverts, nidx, idx_at, bytes, off;
    u32 lo[VTX_ARRAYS], hi[VTX_ARRAYS];
    const VtxLayout* l;
    const u32* m;
    VcEntry* e = vc_new(paddr, size);
    VcKey* c;

    if (e == NULL) {
        return NULL;
    }
    c = &vc_keys[e - vc_entries];
    c->vcd_lo = s->vcd_lo;
    c->vcd_hi = s->vcd_hi;
    // Read before the memory, so a write during the conversion makes the entry stale.
    e->stamp = ps3_mem_stamp();
    n = gp_list_draws(s, list, size, vc_draws, VC_MAX_DRAWS);
    for (i = 1; i < n; i++) {
        if (vc_draws[i].vat != vc_draws[0].vat || prim_family(vc_draws[i].prim) != prim_family(vc_draws[0].prim)) {
            break;
        }
    }
    if (n == 0 || i < n) {
        return e;
    }
    l = current_layout(s, vc_draws[0].vat);
    if (!(l->attrs & (1u << VA_POS))) {
        return e;
    }
    if (!vc_convert(s, l, n, &nverts, &nidx, &idx_at, &bytes)) {
        if (nverts > BATCH_VERTICES || bytes > VC_STAGE) {
            return e;
        }
        vc_release(e);
        return NULL;
    }
    if (nidx == 0 || !vc_alloc(bytes, &off)) {
        return e;
    }
    for (k = 0; k < VTX_ARRAYS; k++) {
        lo[k] = ~0u;
        hi[k] = 0;
    }
    for (i = 0; i < n; i++) {
        vtx_index_range(l, vc_draws[i].data, vc_draws[i].count, lo, hi);
    }
    for (k = 0; k < VTX_ARRAYS; k++) {
        if (l->extent[k] != 0 && lo[k] <= hi[k]) {
            e->arrays |= (u16) (1u << k);
            e->base[k] = s->array_base[k];
            e->stride[k] = (u8) s->array_stride[k];
            e->read_at[k] = s->array_base[k] + lo[k] * s->array_stride[k];
            e->read_len[k] = (hi[k] - lo[k]) * s->array_stride[k] + l->extent[k];
        }
    }
    dev_write_local((u8*) vc_mem + off, vc_stage, bytes);
    e->cached = true;
    e->vat = vc_draws[0].vat;
    e->prim = vc_draws[0].prim;
    e->family = prim_family(e->prim);
    m = vc_masks(s->vcd_lo, s->vcd_hi);
    c->cached = true;
    c->vat = e->vat;
    c->vat_a = s->vat_a[e->vat] & m[0];
    c->vat_b = s->vat_b[e->vat] & m[1];
    c->vat_c = s->vat_c[e->vat] & m[2];
    c->mtx_a = s->xf_regs[0x18] & m[3];
    c->mtx_b = s->xf_regs[0x19] & m[4];
    e->off = off;
    e->bytes = bytes;
    e->idx_at = idx_at;
    e->nidx = nidx;
    e->ndraws = n;
    e->nverts = nverts;
    vc_order[(vc_order_first + vc_order_count++) % VC_ENTRIES] = (s32) (e - vc_entries);
    return e;
}

#ifdef GX_VCACHE_CHECK
// Check build (host): each cached draw converts its list again and compares the bytes with the cached ones.
static void vc_check(GpState* s, const VcEntry* e, const u8* list, u32 size)
{
    static u32 hits, errors;
    u32 n = gp_list_draws(s, list, size, vc_draws, VC_MAX_DRAWS), nverts = 0, nidx = 0, idx_at = 0, bytes = 0;
    bool same = n == e->ndraws && vc_convert(s, current_layout(s, e->vat), n, &nverts, &nidx, &idx_at, &bytes) &&
                bytes == e->bytes && nidx == e->nidx && memcmp(vc_stage, (u8*) vc_mem + e->off, bytes) == 0;

    hits++;
    if (!same && ++errors <= 20) {
        ps3_log("vcache check: list %08x size %u differs (%u draws, %u bytes; cached %u, %u)\n", e->paddr, e->size, n,
                bytes, e->ndraws, e->bytes);
    }
    if (hits % 100000 == 0) {
        ps3_log("vcache check: %u cached draws, %u differ\n", hits, errors);
    }
}
#endif

// A/B, the black flicker detector: each cached list draw's setup, less the matrices, keyed by list and its count in
// the GX frame. A setup that changes for one frame and then comes back goes to the log with what changed.
#define WATCH_SLOTS 8192
enum { W_DRAWN, W_VP, W_FP, W_TEX, W_ZMODE, W_CMODE, W_ALPHA, W_CULL, W_VIEW, W_CHAN, W_LIGHT, W_TEV, W_COUNT };

typedef struct WatchEntry {
    u32 paddr, size, occ, frame;
    u32 s[2][W_COUNT]; // two frames back, then the last frame
    float tev[2][XF_TEV_CONSTS][4];
    u8 n; // setups held
} WatchEntry;

static WatchEntry watch[WATCH_SLOTS];

static void watch_setup(const GpState* s, bool drawn, u32* w)
{
    u32 h = 2166136261u;
    int i;

    memset(w, 0, sizeof(u32) * W_COUNT);
    w[W_DRAWN] = drawn;
    if (!drawn) {
        return;
    }
    w[W_VP] = (u32) (uintptr_t) memo.v;
    w[W_FP] = (u32) (uintptr_t) memo.f;
    w[W_TEX] = hash_words(bound, sizeof(bound), h);
    w[W_ZMODE] = s->bp[0x40] & 0xFFFFFF;
    w[W_CMODE] = s->bp[0x41] & 0xFFFFFF;
    w[W_ALPHA] = s->bp[0xF3] & 0xFFFFFF;
    w[W_CULL] = (s->bp[0x00] >> 14) & 3;
    w[W_VIEW] = hash_words(&s->xf_regs[0x1A], 24, hash_words(&s->bp[0x20], 8, h ^ s->bp[0x59]));
    w[W_CHAN] = hash_words(vc[C_CHAN], 64, h);
    for (i = 0; i < 8; i++) {
        h = hash_words(vc[C_LIGHT + LIGHT_STRIDE * i], 16, h);
    }
    w[W_LIGHT] = h;
    w[W_TEV] = hash_words(memo.tev, sizeof(memo.tev), 2166136261u ^ memo.tev_mask);
}

static void watch_draw(const GpState* s, VcEntry* e, bool drawn)
{
    static const char* const names[W_COUNT] = { "drawn", "VP",   "FP",   "textures", "zmode",  "cmode0",
                                                "alpha", "cull", "view", "channels", "lights", "TEV values" };
    static u32 found, lines, last;
    u32 cur[W_COUNT], occ, h, i, k;
    WatchEntry* w = NULL;

    if (!(ps3_diag.gx_ab & 2097152)) {
        return;
    }
    if (e->watch_frame != gx_frame) {
        e->watch_frame = gx_frame;
        e->watch_occ = 0;
    }
    occ = e->watch_occ++;
    h = (e->paddr >> 5) * 2654435761u ^ e->size * 40503u ^ occ * 2246822519u;
    for (i = 0; i < 64; i++) {
        WatchEntry* c = &watch[(h + i) % WATCH_SLOTS];

        if (c->n == 0 && c->frame == 0) {
            c->paddr = e->paddr;
            c->size = e->size;
            c->occ = occ;
            w = c;
            break;
        }
        if (c->paddr == e->paddr && c->size == e->size && c->occ == occ) {
            w = c;
            break;
        }
    }
    if (w == NULL || w->frame == gx_frame) {
        return;
    }
    watch_setup(s, drawn, cur);
    if (w->n > 0 && w->frame + 1 != gx_frame) {
        w->n = 0;
    }
    if (w->n == 2) {
        if (memcmp(cur, w->s[0], sizeof(cur)) == 0 && memcmp(w->s[1], w->s[0], sizeof(cur)) != 0) {
            char buf[400];
            int len;

            found++;
            // Fighters' colour animations alone give about 20 lines a second (host).
            if (lines < 60) {
                lines++;
                len = snprintf(buf, sizeof(buf), "watch: frame %u list %08x (at %p) size %u #%u, for one frame:",
                               gx_frame - 1, e->paddr, OSPhysicalToCached(e->paddr), e->size, occ);
                for (k = 0; k < W_COUNT && len < (int) sizeof(buf) - 40; k++) {
                    if (w->s[1][k] != w->s[0][k]) {
                        len += snprintf(buf + len, sizeof(buf) - len, " %s %x->%x", names[k], w->s[0][k], w->s[1][k]);
                    }
                }
                ps3_log("%s\n", buf);
                // The TEV value rows that changed (K0-K3, PREV and REG0-REG2 start values, fog), in 1/255 steps.
                for (k = 0; k < XF_TEV_CONSTS; k++) {
                    const float* a = w->tev[0][k];
                    const float* b = w->tev[1][k];

                    if (memcmp(a, b, 16) != 0) {
                        ps3_log("watch:   TEV row %u: %d %d %d %d -> %d %d %d %d\n", k, (int) (a[0] * 255.0f),
                                (int) (a[1] * 255.0f), (int) (a[2] * 255.0f), (int) (a[3] * 255.0f),
                                (int) (b[0] * 255.0f), (int) (b[1] * 255.0f), (int) (b[2] * 255.0f),
                                (int) (b[3] * 255.0f));
                    }
                }
            }
        }
        memcpy(w->s[0], w->s[1], sizeof(cur));
        memcpy(w->s[1], cur, sizeof(cur));
        memcpy(w->tev[0], w->tev[1], sizeof(w->tev[0]));
        memcpy(w->tev[1], memo.tev, sizeof(w->tev[1]));
    } else {
        memcpy(w->tev[w->n], memo.tev, sizeof(w->tev[0]));
        memcpy(w->s[w->n++], cur, sizeof(cur));
    }
    w->frame = gx_frame;
    if (gx_frame - last >= 60) {
        ps3_log("watch: %u one-frame setup changes in the last %u frames\n", found, gx_frame - last);
        found = lines = 0;
        last = gx_frame;
    }
}

static bool call_list(GpState* s, u32 paddr, const u8* list, u32 size, u32* draws, u32* vertices)
{
    const VtxLayout* l;
    VcEntry* e;
    uint64_t t0, t;

    if (vc_mem == NULL || (ps3_diag.gx_ab & (1 | 1024))) {
        return false;
    }
    t0 = t = ps3_prof_now();
    e = vc_find(s, paddr, size);
    if (e != NULL && !vc_current(e)) {
        vc_drop(e);
        e = NULL;
    }
    if (e == NULL) {
        // A heap eviction may wait for the RSX: the open batch goes first.
        batch_flush();
        e = vc_build(s, paddr, list, size);
        ps3_prof.list_builds++;
    }
    if (e == NULL || !e->cached) {
        ps3_prof.list_runs++;
        return false;
    }
#ifdef GX_VCACHE_CHECK
    vc_check(s, e, list, size);
#endif
    batch_flush();
    stats.draws += e->ndraws;
    stats.vertices += e->nverts;
    *draws += e->ndraws;
    *vertices += e->nverts;
    ps3_diag.gp_draw = stats.draws;
    ps3_prof.list_hits++;
    // The lookup and any conversion count as vertex work.
    ps3_prof_lap(PROF_DRAW_VTX, &t);
    l = draw_setup(s, e->prim, e->vat, e->nverts, &t);
    watch_draw(s, e, l != NULL);
    if (l == NULL) {
        stats.dropped_draws += e->ndraws;
        return true;
    }
    ps3_diag.gp_site = "draw: cached list";
    bind_attribs(l, (vc_mem_off + e->off) | DEV_LOCAL);
    before_rsx_draw();
    dev_draw_indexed(e->family, (vc_mem_off + e->off + e->idx_at) | DEV_LOCAL, e->nidx);
    stats.rsx_draws++;
    e->frame = frame;
    after_rsx_draw(true);
    ps3_prof_lap(PROF_DRAW_VTX, &t);
    ps3_prof.ticks[PROF_DRAW] += t - t0;
    return true;
}

// EFB copies

// The colour matrix of a copy to texture format fmt (copy_gen): columns per output channel, then add, mask, 1/mask.
static bool copy_colmat(u32 fmt, bool intensity, float m[28])
{
    static const float ky[3] = { 0.257f, 0.504f, 0.098f };
    float bits = 255.0f;
    int ch, k;

    memset(m, 0, 28 * sizeof(float));
    if (intensity && fmt <= 3) {
        bits = (fmt == 0 || fmt == 2) ? 15.0f : 255.0f;
        for (ch = 0; ch < 4; ch++) {
            if (ch == 3 && fmt >= 2) {
                m[12 + 3] = 1.0f;
                continue;
            }
            for (k = 0; k < 3; k++) {
                m[4 * ch + k] = ky[k];
            }
            m[16 + ch] = 16.0f / 255.0f;
        }
    } else {
        switch (fmt) {
        case 6: // RGBA8
        case 5: // RGB5A3
            for (ch = 0; ch < 4; ch++) {
                m[5 * ch] = 1.0f;
            }
            bits = fmt == 5 ? 31.0f : 255.0f;
            break;
        case 4: // RGB565
            for (ch = 0; ch < 3; ch++) {
                m[5 * ch] = 1.0f;
            }
            m[19] = 1.0f;
            bits = 31.0f;
            break;
        case 0: // R4
        case 1: // R8
        case 8:
            for (ch = 0; ch < 4; ch++) {
                m[4 * ch] = 1.0f;
            }
            bits = fmt == 0 ? 15.0f : 255.0f;
            break;
        case 2: // RA4
        case 3: // RA8
            for (ch = 0; ch < 3; ch++) {
                m[4 * ch] = 1.0f;
            }
            m[15] = 1.0f;
            bits = fmt == 2 ? 15.0f : 255.0f;
            break;
        case 7: // A8
        case 9: // G8
        case 10: // B8
            for (ch = 0; ch < 4; ch++) {
                m[4 * ch + (fmt == 7 ? 3 : fmt == 9 ? 1 : 2)] = 1.0f;
            }
            break;
        case 11: // RG8
        case 12: // GB8
            for (ch = 0; ch < 3; ch++) {
                m[4 * ch + fmt - 11] = 1.0f;
            }
            m[12 + fmt - 10] = 1.0f;
            break;
        default:
            return false;
        }
    }
    for (ch = 0; ch < 4; ch++) {
        m[20 + ch] = bits;
        m[24 + ch] = 1.0f / bits;
    }
    if (fmt == 4 && !intensity) {
        m[21] = 63.0f;
        m[25] = 1.0f / 63.0f;
    }
    return true;
}

static const FpEntry* copy_program(const float colmat[28], bool depth)
{
    static struct {
        float m[28];
        bool depth;
        FpEntry e;
        bool used;
    } cache[16];
    static int next;
    int i;

    for (i = 0; i < 16; i++) {
        if (cache[i].used && cache[i].depth == depth && memcmp(cache[i].m, colmat, sizeof(cache[i].m)) == 0) {
            return &cache[i].e;
        }
    }
    i = next;
    next = (next + 1) % 16;
    if (cache[i].used) {
        dev_finish();
        dev_local_free(cache[i].e.mem);
        cache[i].used = false;
    }
    memcpy(cache[i].m, colmat, sizeof(cache[i].m));
    cache[i].depth = depth;
    if (fp_build(&cache[i].e, copy_gen(colmat, depth, &fp_scratch)) == NULL) {
        return NULL;
    }
    cache[i].used = true;
    return &cache[i].e;
}

// Draws the src rectangle of the EFB's colour, or of its depth buffer, into all of dst (dw x dh).
static void copy_pass(const DevTarget* dst, u32 sx, u32 sy, u32 sw, u32 sh, const FpEntry* fp, bool linear,
                      bool depth)
{
    static const float clip[4][2] = { { -1, 1 }, { 1, 1 }, { 1, -1 }, { -1, -1 } };
    float scale[4] = { dst->width * 0.5f, dst->height * -0.5f, 0.5f, 0.0f };
    float offset[4] = { dst->width * 0.5f, dst->height * 0.5f, 0.5f, 0.0f };
    float u0 = (float) sx / EFB_WIDTH, u1 = (float) (sx + sw) / EFB_WIDTH;
    float v0 = (float) sy / EFB_HEIGHT, v1 = (float) (sy + sh) / EFB_HEIGHT;
    const float tc[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
    CopyVertex* q;
    DevTexture t;
    u32 io;
    int i;

    q = dev_ring_alloc(4 * sizeof(CopyVertex), &io);
    if (q == NULL) {
        return;
    }
    for (i = 0; i < 4; i++) {
        q[i].pos[0] = clip[i][0];
        q[i].pos[1] = clip[i][1];
        q[i].pos[2] = 0.5f;
        q[i].pos[3] = 1.0f;
        q[i].tc[0] = tc[i][0];
        q[i].tc[1] = tc[i][1];
    }
    // The EFB is read as a texture: the draws into it must be done.
    dev_barrier();
    // A/B: no texture unit holds the destination while the copy renders into it, as Dolphin's PR 1689 (D3D).
    if (ps3_diag.gx_ab & 65536) {
        for (i = 1; i < 8; i++) {
            dev_texture(i, NULL);
        }
    }
    dev_target(dst);
    efb_bound = false;
    dev_viewport(scale, offset);
    dev_scissor(0, 0, dst->width, dst->height);
    dev_depth(false, DEV_FUNC_NEVER + 7, false);
    dev_blend(false, DEV_ONE, DEV_ZERO, DEV_EQ_ADD);
    dev_alpha_test(false, 0, 0);
    dev_color_mask(true, true);
    dev_cull(false, false);
    memset(&t, 0, sizeof(t));
    t.offset = depth ? efb_target.depth_offset : efb_target.color_offset;
    t.pitch = EFB_PITCH;
    t.width = EFB_WIDTH;
    t.height = EFB_HEIGHT;
    t.levels = 1;
    t.wrap_s = t.wrap_t = DEV_WRAP_CLAMP;
    // Depth bytes cannot be filtered.
    t.min_filter = t.mag_filter = linear && !depth ? DEV_FILTER_LINEAR : DEV_FILTER_NEAREST;
    dev_texture(0, &t);
    tex_bind(0, NULL);
    use_vp(&copy_vp);
    use_fp(fp);
    for (i = 0; i < 16; i++) {
        dev_attrib(i, 0, 0, 0, VTX_F32);
    }
    dev_attrib(VA_POS, io, sizeof(CopyVertex), 4, VTX_F32);
    dev_attrib(VA_TEX0, io + 16, sizeof(CopyVertex), 2, VTX_F32);
    bound_layout = NULL;
    dev_draw(DEV_PRIM_QUADS, 4);
    // Later draws may sample dst.
    dev_barrier();
}

static Xfb* xfb_for(u32 addr)
{
    Xfb* x;
    int i;

    for (i = 0; i < XFBS; i++) {
        if (xfbs[i].mem != NULL && xfbs[i].addr == addr) {
            return &xfbs[i];
        }
    }
    x = &xfbs[xfb_next];
    xfb_next = (xfb_next + 1) % XFBS;
    if (x->mem == NULL) {
        x->mem = dev_local_alloc(128, EFB_PITCH * XFB_MAX_HEIGHT, &x->offset);
        if (x->mem == NULL) {
            return NULL;
        }
    }
    x->addr = addr;
    return x;
}

// A/B: reads a fighter's shadow copy back once the RSX wrote it, as the black floor's suspect. Its 2-pixel border is
// always white on the GameCube; the inside is white but for the fighter. The PPE waits for the RSX at each one.
static void check_shadow(const TexEntry* e)
{
    static u32 copies, dark, dark_edge, lines, last;
    const volatile u8* m = e->mem;
    u32 x, y, in = 0, in_dark = 0, edge = 0, edge_dark = 0, sum = 0;

    dev_finish();
    for (y = 8; y < 248; y += 8) {
        for (x = 8; x < 248; x += 8) {
            u32 r = (*(const volatile u32*) (m + y * e->pitch + x * 4) >> 16) & 0xFF;

            in++;
            in_dark += r < 64;
            sum += r;
        }
    }
    for (x = 0; x < 256; x += 8) {
        u32 r[4] = { *(const volatile u32*) (m + x * 4), *(const volatile u32*) (m + 255 * e->pitch + x * 4),
                     *(const volatile u32*) (m + x * e->pitch), *(const volatile u32*) (m + x * e->pitch + 255 * 4) };
        int k;

        for (k = 0; k < 4; k++) {
            edge++;
            edge_dark += ((r[k] >> 16) & 0xFF) < 64;
        }
    }
    copies++;
    dark += in_dark * 2 > in;
    dark_edge += edge_dark * 10 > edge;
    if ((in_dark * 2 > in || edge_dark * 10 > edge) && lines < 30) {
        lines++;
        ps3_log("shadow check: present %u copy to %08x dark: inside %u of %u (mean %u), border %u of %u\n",
                stats.presents, e->addr, in_dark, in, sum / in, edge_dark, edge);
    }
    if (stats.presents - last >= 60) {
        ps3_log("shadow check: %u copies, %u mostly dark inside, %u with a dark border\n", copies, dark, dark_edge);
        copies = dark = dark_edge = lines = 0;
        last = stats.presents;
    }
}

static void efb_copy(GpState* s)
{
    u32 v = s->bp[BP_EFB_COPY], tl = s->bp[0x49], wh = s->bp[0x4A];
    u32 sx = tl & 0x3FF, sy = (tl >> 10) & 0x3FF, sw = (wh & 0x3FF) + 1, sh = ((wh >> 10) & 0x3FF) + 1;
    u32 addr = (s->bp[0x4B] & 0xFFFFFF) << 5;
    uint64_t t = ps3_prof_now();

    batch_flush();
    stats.efb_copies++;
    if (trace_left != 0) {
        trace_left--;
        ps3_log("backend: EFB copy %#x from (%u, %u) %ux%u to %08x\n", v, sx, sy, sw, sh, addr);
    }
    if (sx + sw > EFB_WIDTH) {
        sw = EFB_WIDTH - sx;
    }
    if (sy + sh > EFB_HEIGHT) {
        sh = EFB_HEIGHT - sy;
    }
    if ((v >> 14) & 1) {
        // The GX frame ends here, not at the present, which may come during the next frame's draws.
        frame_draws = 0;
        gx_frame++;
        // Display copy: an RSX surface for the XFB at addr, the lines scaled as the copy's Y scale says.
        u32 ys = s->bp[0x4E] & 0x1FF;
        float yscale = ys == 0 ? 1.0f : ((v >> 10) & 1) ? 256.0f / (float) ys : (float) ys / 256.0f;
        u32 lines = (u32) (1.0f + (float) (sh - 1) * yscale);
        Xfb* x = xfb_for(addr);

        lines = lines > XFB_MAX_HEIGHT ? XFB_MAX_HEIGHT : lines;
        if (x != NULL) {
            static const float ident[28] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0,
                                             255, 255, 255, 255, 1 / 255.0f, 1 / 255.0f, 1 / 255.0f, 1 / 255.0f };
            DevTarget t = { x->offset, EFB_PITCH, 0, 0, (u16) sw, (u16) lines };
            const FpEntry* fp = copy_program(ident, false);

            x->width = (u16) sw;
            x->height = (u16) lines;
            if (fp != NULL) {
                copy_pass(&t, sx, sy, sw, sh, fp, lines != sh, false);
            }
            stats.xfb_copies++;
        }
    } else {
        // Texture copy: an RSX texture registered for addr, which texture loads at addr then use.
        u32 tpf = (v >> 3) & 0xF, fmt = tpf / 2 + (tpf & 1) * 8;
        bool intensity = (v >> 15) & 1, half = (v >> 9) & 1;
        u32 dw = half ? (sw + 1) / 2 : sw, dh = half ? (sh + 1) / 2 : sh;
        float colmat[28];
        TexEntry* e;
        // PE pixel format Z24: GXCopyTex of a z format, the formats as for colour (Dolphin).
        bool depth = (s->bp[0x43] & 7) == 3;

        if (!copy_colmat(fmt, intensity && !depth, colmat)) {
            unsupported(depth ? U_DEPTH_COPY : U_COPY_FORMAT, depth ? "a depth copy format" : "an EFB copy format");
        } else {
            const FpEntry* fp = copy_program(colmat, depth);

            e = find_efb_texture(addr, dw, dh);
            if (e == NULL) {
                e = tex_slot();
                memset(e, 0, sizeof(*e));
                e->pitch = (dw * 4 + 63) & ~63u;
                e->mem = tex_alloc(e->pitch * dh, &e->offset);
                if (e->mem != NULL) {
                    e->used = true;
                    e->efb = true;
                    e->addr = addr;
                    e->width = dw;
                    e->height = dh;
                    e->levels = 1;
                    tex_link(e);
                }
            }
            if (e != NULL && e->used && fp != NULL) {
                DevTarget t = { e->offset, e->pitch, 0, 0, (u16) dw, (u16) dh };

                e->frame = frame;
                copy_pass(&t, sx, sy, sw, sh, fp, half, depth);
                // lbshadow.c's shadows are 256x256.
                if ((ps3_diag.gx_ab & 131072) && dw == 256 && dh == 256) {
                    check_shadow(e);
                }
            }
        }
    }
    if ((v >> 11) & 1) {
        u32 ar = s->bp[0x4F], gb = s->bp[0x50], cm0 = s->bp[0x41];
        u32 argb = ((ar >> 8) & 0xFF) << 24 | (ar & 0xFF) << 16 | ((gb >> 8) & 0xFF) << 8 | (gb & 0xFF);

        if (!efb_has_alpha(s)) {
            argb |= 0xFF000000;
        }
        dev_target(&efb_target);
        efb_bound = true;
        dev_scissor(sx, sy, sw, sh);
        dev_clear((cm0 >> 3) & 1, ((cm0 >> 4) & 1) && efb_has_alpha(s), (s->bp[0x40] >> 4) & 1, argb,
                  s->bp[0x51] & 0xFFFFFF);
    }
    // The pass changed the RSX state the GX draws keep, and a texture at addr may now be the copy.
    fp_current = NULL;
    memo.state_ok = false;
    tex_forget();
    ps3_prof_lap(PROF_COPY, &t);
}

static void tlut_load(GpState* s)
{
    u32 addr = (s->bp[0x64] & 0xFFFFFF) << 5, t = s->bp[BP_LOAD_TLUT1];
    u32 off = (t & 0x3FF) << 9, bytes = ((t >> 10) & 0x7FF) << 5;
    const u8* src = OSPhysicalToCached(addr);

    if (src == NULL || off >= sizeof(tmem_tlut)) {
        return;
    }
    if (off + bytes > sizeof(tmem_tlut)) {
        bytes = sizeof(tmem_tlut) - off;
    }
    memcpy(tmem_tlut + off, src, bytes);
    tlut_loads++;
}

static void tex_invalidate(GpState* s)
{
    // Textures are found by content, so a TMEM invalidate changes nothing here.
    (void) s;
}

// The textures read from [addr, addr + size) go, so the next use decodes the new data (movie frames).
static void mem_changed(GpState* s, u32 addr, u32 size)
{
    bool waited = false;
    int i;

    (void) s;
    batch_flush();
    for (i = 0; i < TEX_CACHE; i++) {
        TexEntry* e = &tex_cache[i];

        if (!e->used || e->efb || e->addr >= addr + size || addr >= e->addr + e->bytes) {
            continue;
        }
        // Earlier draws may still sample it.
        if (!waited) {
            dev_finish();
            waited = true;
        }
        tex_free(e);
    }
    if (waited) {
        tex_forget();
    }
}

static void finish(GpState* s)
{
    (void) s;
    batch_flush();
    if (trace_left != 0) {
        ps3_log("backend: finish %u waits for the RSX\n", frame);
    }
    dev_finish();
    if (trace_left != 0) {
        ps3_log("backend: finish %u done\n", frame);
    }
    frame++;
}

static void present(GpState* s, u32 xfb)
{
    gx_rsx_present(xfb);
}

static const GpBackend backend = { draw, efb_copy, tlut_load, tex_invalidate, finish, present, mem_changed,
                                   call_list };

const GpBackend* gx_rsx_backend(void)
{
    return &backend;
}

bool gx_rsx_init(void)
{
    u32 off;
    VpSrc z;

    if (initialized) {
        return true;
    }
    if (!dev_init()) {
        return false;
    }
    fp_set_text(false);
    vp_set_text(false);
    efb_color = dev_local_alloc(128, EFB_PITCH * EFB_HEIGHT, &efb_target.color_offset);
    // As tall as the largest XFB copy, whose target borrows this depth buffer.
    efb_depth = dev_local_alloc(128, EFB_PITCH * XFB_MAX_HEIGHT, &off);
    tex_temp = memalign(128, MAX_TEX_SIZE * MAX_TEX_SIZE * 4);
    tex_swz = memalign(128, MAX_TEX_SIZE * MAX_TEX_SIZE * 4 * 4 / 3 + 64);
    if (efb_color == NULL || efb_depth == NULL || tex_temp == NULL || tex_swz == NULL) {
        OSReport("GX RSX: no memory for the EFB and texture buffers\n");
        return false;
    }
    memset(tex_bucket, 0xFF, sizeof(tex_bucket));
    // Without its memory the display list cache stays off: lists run as commands.
    vc_reset();
    vc_mem = dev_local_alloc(128, VC_HEAP, &vc_mem_off);
    vc_stage = memalign(128, VC_STAGE);
    if (vc_mem == NULL || vc_stage == NULL) {
        OSReport("GX RSX: no memory for the display list cache\n");
        dev_local_free(vc_mem);
        vc_mem = NULL;
    }
    efb_target.color_pitch = EFB_PITCH;
    efb_target.depth_offset = off;
    efb_target.depth_pitch = EFB_PITCH;
    efb_target.width = EFB_WIDTH;
    efb_target.height = EFB_HEIGHT;
    // The copy pass program: position and texcoord 0 as they are.
    z = vp_t(0, NULL);
    vp_init(&vp_scratch);
    vp_op(&vp_scratch, VOP_MOV, VP_OUT(VO_POS), 0xF, false, vp_in(VA_POS, NULL), z, z);
    vp_op(&vp_scratch, VOP_MOV, VP_OUT(VO_TC0), 0xF, false, vp_in(VA_TEX0, NULL), z, z);
    vp_end(&vp_scratch);
    copy_vp.ucode = malloc(16u * vp_scratch.n);
    memcpy(copy_vp.ucode, vp_scratch.w, 16u * vp_scratch.n);
    copy_vp.n = (u16) vp_scratch.n;
    copy_vp.slot = -1;
    copy_vp.regs = 1;
    copy_vp.inputs = vp_scratch.input_mask;
    copy_vp.outputs = vp_scratch.output_mask;
    {
        static FpEntry plain;

        if (fp_build(&plain, plain_gen(&fp_scratch)) == NULL) {
            return false;
        }
        dev_set_present_programs((const u32(*)[4]) copy_vp.ucode, copy_vp.n, copy_vp.inputs, copy_vp.outputs,
                                 plain.offset, plain.regs, plain.texcoords);
    }
    // A black EFB with the far depth, alpha 1, as an RGB8 EFB reads.
    dev_target(&efb_target);
    efb_bound = true;
    dev_scissor(0, 0, EFB_WIDTH, EFB_HEIGHT);
    dev_color_mask(true, true);
    dev_clear(true, true, true, 0xFF000000, 0xFFFFFF);
    // The ring draws' vertices from the SPU, written before the RSX may read them.
    vx_init();
    dev_set_flush_hook(vx_drain);
    initialized = true;
    return true;
}

void gx_rsx_present(u32 xfb)
{
    uint64_t t = ps3_prof_now();
    GxRsxScreen screen = { false, { NULL, 0, 0, 0 } };
    int i;

    batch_flush();
    if (screen_fn != NULL) {
        screen_fn(&screen);
    }
    // From the next frame on: a present uploads all constants again (vc_valid).
    xf_widescreen = screen.wide;
    for (i = 0; i < XFBS; i++) {
        if (xfbs[i].mem != NULL && xfbs[i].addr == xfb) {
            dev_barrier();
#ifdef PS3_DUMP_FRAMES
            if ((stats.presents + 1) % PS3_DUMP_FRAMES == 0 && stats.presents < 6 * PS3_DUMP_FRAMES) {
                dev_debug_dump("xfb", stats.presents + 1, xfbs[i].mem, EFB_PITCH, xfbs[i].width, xfbs[i].height);
            }
#endif
            dev_present(xfbs[i].offset, EFB_PITCH, xfbs[i].width, xfbs[i].height, screen.wide, &screen.overlay);
            if (ps3_diag.gx_ab & 512) {
                dev_finish();
            }
            stats.presents++;
            // The present pass leaves its own state, and the console's flip runs gcm's state reset (all texture units
            // off). The present can come mid-frame, so every binding goes; A/B: the old way, which drew those black.
            efb_bound = false;
            fp_current = NULL;
            vp_current = NULL;
            bound_layout = NULL;
            if (ps3_diag.gx_ab & 16777216) {
                textures_bound = false;
            } else {
                tex_forget();
            }
            vc_valid = false;
            memo.state_ok = false;
            // The XMB draws at the flip with its own vertex program, over the loaded ones (the libgcm way loads
            // the program at each set, so the system need not keep them); the constants go up again already.
            if (overlay_on) {
                overlay_presents = OVERLAY_TAIL;
            }
            if (overlay_presents != 0) {
                overlay_presents--;
                vp_forget();
            }
            ps3_prof_lap(PROF_PRESENT, &t);
            return;
        }
    }
}

void gx_rsx_set_overlay(bool on)
{
    overlay_on = on;
}

void gx_rsx_set_screen(void (*fn)(GxRsxScreen* out))
{
    screen_fn = fn;
}

void gx_rsx_get_stats(GxRsxStats* out)
{
    *out = stats;
}
