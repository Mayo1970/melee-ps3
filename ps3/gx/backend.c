// The GP backend on the RSX. Register meanings as Dolphin's BPMemory/XFMemory; state mapping after DolphinPS3's
// VertexManager. Runs on the GP thread only.
#include "backend.h"

#include <malloc.h>
#include <string.h>

#include <dolphin/os.h>

#include "../os/diag.h"
#include "../os/prof.h"
#include "../os/udplog.h"
#include "fpgen.h"
#include "rsxdev.h"
#include "tevgen.h"
#include "texdec.h"
#include "vpgen.h"
#include "vtx.h"
#include "xfgen.h"

#define EFB_PITCH (EFB_WIDTH * 4)
#define FP_CACHE 2048
#define VP_CACHE 1024
#define LAYOUTS 64
#define TEX_CACHE 512
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
    U_ZTEX = 1 << 4,
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
} FpEntry;

typedef struct VpEntry {
    bool used;
    u32 hash;
    XfKey key;
    u32 (*ucode)[4];
    u16 n, regs;
    u32 inputs, outputs;
} VpEntry;

typedef struct TexEntry {
    bool used, efb;
    u32 addr, fmt, width, height, levels, tlut_fmt;
    u32 bytes; // GX texture data read, all levels
    u32 hash, tlut_hash;
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

static float vc[C_COUNT][4];
static float vc_new[C_COUNT][4];
static bool vc_valid;

static FpEntry fp_cache[FP_CACHE];
static FpProg fp_scratch;
static u32 fp_words[FP_MAXI * 8];
static const FpEntry* fp_current;

static VpEntry vp_cache[VP_CACHE];
static VpProg vp_scratch;
static const VpEntry* vp_current;
static VpEntry copy_vp;

static VtxLayout layouts[LAYOUTS];
static int nlayouts, layout_next;

static TexEntry tex_cache[TEX_CACHE];
// Per GX map: the registers of the last lookup, so a texture is hashed once per frame and not per draw.
static struct {
    u32 regs[5], frame, tlut_loads;
    DevTexture d;
    const TexEntry* e;
} tex_memo[8];
static u32 tlut_loads;
static u32* tex_temp;
static u32* tex_swz;
static u8 tmem_tlut[0x80000];
static u32 frame;

static Xfb xfbs[XFBS];
static int xfb_next;

// Step 3 diagnostics: the last RSX draws. The RSX marks each one done, so a hang names its draw.
typedef struct DrawRecord {
    u32 seq;
    const char* kind;
    u32 prim, count, attrs, out_size;
    u8 attr[16][3]; // offset, count, type
    const VpEntry* vp;
    const FpEntry* fp;
    u32 target, target_w, target_h, tex_on;
    DevTexture tex[8];
} DrawRecord;

static DrawRecord records[16];
static u32 draw_seq;

// What the last draw derived from the registers, valid while the GpState generations it read stay.
static struct {
    bool layout_ok, tev_ok, vp_ok, state_ok, tex_ok, consts_ok;
    u32 layout_cp;
    int layout_vat;
    VtxLayout* layout;
    u32 tev_bp, tev_xf;
    const FpEntry* f;
    float tev[XF_TEV_CONSTS][4];
    u16 tev_mask;
    u32 vp_xf;
    u16 vp_attrs, vp_tev;
    bool vp_pos3;
    const VpEntry* v;
    // The RSX raster state as apply_state left it, and its result.
    u32 state_bp, state_xf;
    bool state_pass;
    u32 tex_bp, tex_frame, tex_tluts;
} memo;

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
    fp_current = NULL;
    memo.tev_ok = false;
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

static const VpEntry* vp_get(const XfKey* key)
{
    u32 h = hash_bytes(key, sizeof(*key), 2166136261u), i;

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
            e->regs = (u16) (vp_scratch.max_temp + 1);
            e->inputs = vp_scratch.input_mask;
            e->outputs = vp_scratch.output_mask;
            e->hash = h;
            e->key = *key;
            e->used = true;
            stats.vertex_programs++;
            return e;
        }
        if (e->hash == h && memcmp(&e->key, key, sizeof(*key)) == 0) {
            return e;
        }
    }
    OSReport("GX RSX: vertex program cache full\n");
    return NULL;
}

static void use_vp(const VpEntry* v)
{
    if (v != vp_current) {
        dev_vertex_program((const u32(*)[4]) v->ucode, v->n, v->inputs, v->outputs, v->regs);
        vp_current = v;
        stats.vp_loads++;
    }
}

static void use_fp(const FpEntry* f)
{
    if (f != fp_current) {
        dev_fragment_program(f->offset, f->regs, f->texcoords, f->texcoords4);
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

// The TEV config for the current BP registers, with the per-draw values on free texcoord interpolants.
static void build_tev(const GpState* s, int numtexgens, TevConfig* cfg, float vals[XF_TEV_CONSTS][4], u16* mask)
{
    u8 tables[4][4];
    float kval[4][4], reg[4][4], fog_param[4], fog_color[4];
    u32 regs_read, konst_read;
    int i, slot = numtexgens;
    bool ortho;

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
        }
        if (slot < 8) {
            cfg->fog_cin = (s8) (FIN_TC0 + slot++);
            memcpy(vals[9], fog_color, 16);
            *mask |= 1u << 9;
        } else {
            memcpy(cfg->fog_color, fog_color, 16);
        }
    }
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

// The single-compare forms of the GX alpha test. False when no pixel can pass.
static bool apply_alpha_test(const GpState* s)
{
    u32 at = s->bp[0xF3], ref0 = at & 0xFF, ref1 = (at >> 8) & 0xFF;
    u32 c0 = (at >> 16) & 7, c1 = (at >> 19) & 7, logic = (at >> 22) & 3;
    bool k0 = c0 == 0 || c0 == 7, k1 = c1 == 0 || c1 == 7;
    bool v0 = c0 == 7, v1 = c1 == 7;

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
    // The XF viewport less the scissor offset; GX depth moves from [-w, 0] to [0, w] in the program.
    scale[0] = u2f(x[0x1A]);
    scale[1] = u2f(x[0x1B]);
    scale[2] = u2f(x[0x1C]) / 16777215.0f;
    scale[3] = 0.0f;
    // GX samples pixels at 7/12 of a pixel, the RSX at 1/2: as Dolphin, the image moves 1/12 pixel up and left.
    offset[0] = u2f(x[0x1D]) - (float) ox - 1.0f / 12.0f;
    offset[1] = u2f(x[0x1E]) - (float) oy - 1.0f / 12.0f;
    offset[2] = (u2f(x[0x1F]) - u2f(x[0x1C])) / 16777215.0f;
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
    if ((s->bp[0xF5] & 3) != 0) {
        unsupported(U_ZTEX, "a z texture");
    }
    return apply_alpha_test(s);
}

// Constants

static void upload_rows(int first, int count)
{
    int i = first, end = first + count;

    while (i < end) {
        int j;

        if (memcmp(vc[i], vc_new[i], 16) == 0) {
            i++;
            continue;
        }
        for (j = i + 1; j < end && memcmp(vc[j], vc_new[j], 16) != 0; j++) {
        }
        dev_vp_constants((u32) i, (u32) (j - i), &vc_new[i][0]);
        memcpy(vc[i], vc_new[i], 16u * (j - i));
        i = j;
    }
}

static void update_constants(GpState* s, const VtxLayout* l, float tev[XF_TEV_CONSTS][4], u16 tev_mask)
{
    bool xf_dirty = !vc_valid || s->xf_mem_dirty_hi > s->xf_mem_dirty_lo || s->xf_reg_dirty[0] ||
                    s->xf_reg_dirty[1] || s->xf_reg_dirty[2];
    int i;

    // The same layout and TEV values as the last upload: the DQ and TEV rows are on the RSX.
    if (!xf_dirty && memo.consts_ok) {
        return;
    }
    if (xf_dirty) {
        ps3_prof.xf_reloads++;
        xf_constants(s, vc_new);
        s->xf_mem_dirty_lo = XF_MEM_SIZE;
        s->xf_mem_dirty_hi = 0;
        s->xf_reg_dirty[0] = s->xf_reg_dirty[1] = s->xf_reg_dirty[2] = 0;
    }
    for (i = 0; i < 12; i++) {
        vc_new[C_DQ + i / 4][i % 4] = i < 10 ? l->dq[i] : 0.0f;
    }
    for (i = 0; i < XF_TEV_CONSTS; i++) {
        if ((tev_mask >> i) & 1) {
            memcpy(vc_new[C_TEV + i], tev[i], 16);
        }
    }
    if (!vc_valid) {
        // The shadow starts unlike every value, so all of it goes up once.
        memset(vc, 0xFF, sizeof(vc));
        vc_valid = true;
    }
    if (xf_dirty) {
        upload_rows(0, C_COUNT);
    } else {
        upload_rows(C_DQ, 3);
        upload_rows(C_TEV, XF_TEV_CONSTS);
    }
    memo.consts_ok = true;
}

// Vertex layouts

static VtxLayout* get_layout(const GpState* s, int vat)
{
    int i;

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
    if ((s->vcd_lo >> 8) & 1) {
        unsupported(U_TEX7_MTXIDX, "a texcoord 7 matrix index in the vertex");
    }
    return &layouts[i];
}

// Textures

static u32 tex_hash(const u8* p, u32 size)
{
    u32 h = 2166136261u, i, step;

    if (size <= TEX_HASH_FULL) {
        return hash_bytes(p, size, h);
    }
    h = hash_bytes(p, 256, h);
    h = hash_bytes(p + size - 256, 256, h);
    step = (size - 512) / 64;
    for (i = 0; i < 64; i++) {
        h = hash_bytes(p + 256 + i * step, 8, h);
    }
    return h;
}

static void free_textures(bool keep_efb)
{
    int i;

    dev_finish();
    for (i = 0; i < TEX_CACHE; i++) {
        if (tex_cache[i].used && !(keep_efb && tex_cache[i].efb)) {
            dev_local_free(tex_cache[i].mem);
            tex_cache[i].used = false;
        }
    }
    memset(bound, 0, sizeof(bound));
    memset(tex_memo, 0, sizeof(tex_memo));
    textures_bound = false;
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

static TexEntry* tex_slot(void)
{
    int i, oldest = 0;

    for (i = 0; i < TEX_CACHE; i++) {
        if (!tex_cache[i].used) {
            return &tex_cache[i];
        }
        if (tex_cache[i].frame < tex_cache[oldest].frame) {
            oldest = i;
        }
    }
    dev_finish();
    dev_local_free(tex_cache[oldest].mem);
    tex_cache[oldest].used = false;
    memset(tex_memo, 0, sizeof(tex_memo));
    return &tex_cache[oldest];
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
    int i;

    for (i = 0; i < TEX_CACHE; i++) {
        TexEntry* e = &tex_cache[i];

        if (e->used && e->efb && e->addr == addr && e->width == w && e->height == h) {
            return e;
        }
    }
    return NULL;
}

// The texture on GX map unit, from its BP registers; NULL when it cannot be read.
static const TexEntry* tex_get(const GpState* s, int unit, DevTexture* d)
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
    u32 minf = (mode0 >> 5) & 7, maxlod = (mode1 >> 8) & 0xFF, levels = 1, hash, tlut_hash = 0, tlut_fmt = 0;
    u32 tlut_entries = 0, i;
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
            tlut_hash = hash_bytes(tlut, tlut_entries * 2, 2166136261u);
        }
        hash = tex_hash(src, size);
        for (i = 0; i < TEX_CACHE; i++) {
            TexEntry* t = &tex_cache[i];

            if (t->used && !t->efb && t->addr == addr && t->fmt == fmt && t->width == w && t->height == h &&
                t->levels == levels && t->hash == hash && t->tlut_hash == tlut_hash && t->tlut_fmt == tlut_fmt) {
                e = t;
                break;
            }
        }
        if (e == NULL) {
            e = tex_slot();
            memset(e, 0, sizeof(*e));
            e->addr = addr;
            e->fmt = fmt;
            e->width = w;
            e->height = h;
            e->levels = levels;
            e->bytes = size;
            e->hash = hash;
            e->tlut_hash = tlut_hash;
            e->tlut_fmt = tlut_fmt;
            if (!tex_upload(e, src, tlut, tlut_entries)) {
                return NULL;
            }
            e->used = true;
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

static void bind_textures(const GpState* s, const TevConfig* cfg)
{
    static const u8 base[2] = { 0x80, 0xA0 };
    bool used[8] = { false };
    int i, k;

    for (i = 0; i < cfg->nstages; i++) {
        if (cfg->st[i].tex >= 0) {
            used[cfg->st[i].tex] = true;
        }
    }
    for (i = 0; i < 8; i++) {
        u32 regs[5];
        bool same;

        if (!used[i]) {
            continue;
        }
        // Mode 0, mode 1, image 0, image 3, TLUT.
        for (k = 0; k < 5; k++) {
            static const u8 off[5] = { 0x00, 0x04, 0x08, 0x14, 0x18 };
            regs[k] = s->bp[base[i >> 2] + off[k] + (i & 3)];
        }
        same = tex_memo[i].e != NULL && tex_memo[i].frame == frame && tex_memo[i].tlut_loads == tlut_loads &&
               memcmp(tex_memo[i].regs, regs, sizeof(regs)) == 0;
        if (!same) {
            tex_memo[i].e = tex_get(s, i, &tex_memo[i].d);
            memcpy(tex_memo[i].regs, regs, sizeof(regs));
            tex_memo[i].frame = frame;
            tex_memo[i].tlut_loads = tlut_loads;
        }
        if (!same || !textures_bound || bound[i] != tex_memo[i].e) {
            dev_texture(i, tex_memo[i].e != NULL ? &tex_memo[i].d : NULL);
            bound[i] = tex_memo[i].e;
        }
    }
    textures_bound = true;
}

// Draws

static u32 prim_type(int prim)
{
    static const u8 types[8] = { DEV_PRIM_QUADS, DEV_PRIM_QUADS, DEV_PRIM_TRIANGLES, DEV_PRIM_TRIANGLE_STRIP,
                                 DEV_PRIM_TRIANGLE_FAN, DEV_PRIM_LINES, DEV_PRIM_LINE_STRIP, DEV_PRIM_POINTS };
    return types[prim & 7];
}

static DrawRecord* record_draw(const char* kind, u32 prim, u32 count, const DevTarget* target)
{
    DrawRecord* r = &records[++draw_seq % 16];

    memset(r, 0, sizeof(*r));
    r->seq = draw_seq;
    r->kind = kind;
    r->prim = prim;
    r->count = count;
    r->target = target->color_offset;
    r->target_w = target->width;
    r->target_h = target->height;
    return r;
}


static void draw(GpState* s, int prim, int vat, u32 count, const u8* data)
{
    static TevConfig cfg;
    VtxLayout* l;
    const FpEntry* f;
    const VpEntry* v;
    DrawRecord* r;
    XfKey key;
    u32 io, i, numtexgens = s->xf_regs[0x3F] & 0xF;
    u8* out;
    bool tev_hit, reused = true;
    uint64_t t0 = ps3_prof_now(), t = t0;

    stats.draws++;
    stats.vertices += count;
    if (!memo.layout_ok || memo.layout_cp != s->cp_gen || memo.layout_vat != vat) {
        memo.layout = get_layout(s, vat);
        memo.layout_cp = s->cp_gen;
        memo.layout_vat = vat;
        memo.layout_ok = true;
        memo.consts_ok = false;
    }
    l = memo.layout;
    if (!(l->attrs & (1u << VA_POS)) || (((s->bp[0x00] >> 14) & 3) == 3 && prim <= 4)) {
        stats.dropped_draws++;
        return;
    }
    numtexgens = numtexgens > 8 ? 8 : numtexgens;
    ps3_diag.gp_draw = stats.draws;
    ps3_diag.gp_site = "draw: TEV";
    tev_hit = memo.tev_ok && memo.tev_bp == s->bp_gen && memo.tev_xf == s->xf_gen;
    if (!tev_hit) {
        reused = false;
        memo.tev_ok = false;
        memo.consts_ok = false;
        memo.tex_ok = false;
        memset(memo.tev, 0, sizeof(memo.tev));
        build_tev(s, (int) numtexgens, &cfg, memo.tev, &memo.tev_mask);
        memo.f = fp_get(&cfg);
        if (memo.f != NULL) {
            memo.tev_bp = s->bp_gen;
            memo.tev_xf = s->xf_gen;
            memo.tev_ok = true;
        }
    }
    f = memo.f;
    ps3_prof_lap(PROF_DRAW_TEV, &t);
    ps3_diag.gp_site = "draw: XF";
    if (!memo.vp_ok || memo.vp_xf != s->xf_gen || memo.vp_attrs != l->attrs || memo.vp_pos3 != l->pos3 ||
        memo.vp_tev != memo.tev_mask) {
        reused = false;
        xf_make_key(s, l->attrs, l->pos3, memo.tev_mask, &key);
        memo.v = vp_get(&key);
        memo.vp_ok = memo.v != NULL;
        memo.vp_xf = s->xf_gen;
        memo.vp_attrs = l->attrs;
        memo.vp_pos3 = l->pos3;
        memo.vp_tev = memo.tev_mask;
    }
    v = memo.v;
    ps3_prof_lap(PROF_DRAW_XF, &t);
    ps3_diag.gp_site = "draw: state";
    if (f == NULL || v == NULL) {
        stats.dropped_draws++;
        return;
    }
    if (!memo.state_ok || memo.state_bp != s->bp_gen || memo.state_xf != s->xf_gen) {
        reused = false;
        memo.state_pass = apply_state(s);
        memo.state_bp = s->bp_gen;
        memo.state_xf = s->xf_gen;
        memo.state_ok = true;
    }
    if (!memo.state_pass) {
        stats.dropped_draws++;
        return;
    }
    if (reused) {
        ps3_prof.state_reuses++;
    }
    ps3_prof_lap(PROF_DRAW_STATE, &t);
    ps3_diag.gp_site = "draw: vertices";
    out = dev_ring_alloc(count * l->out_size, &io);
    if (out == NULL || !vtx_convert(l, s, data, count, out)) {
        stats.dropped_draws++;
        return;
    }
    ps3_prof_lap(PROF_DRAW_VTX, &t);
    ps3_diag.gp_site = "draw: constants";
    update_constants(s, l, memo.tev, memo.tev_mask);
    ps3_prof_lap(PROF_DRAW_CONST, &t);
    ps3_diag.gp_site = "draw: programs and textures";
    use_vp(v);
    use_fp(f);
    // The textures are checked once per frame and after each TLUT load (bind_textures).
    if (!memo.tex_ok || !textures_bound || memo.tex_bp != s->bp_gen || memo.tex_frame != frame ||
        memo.tex_tluts != tlut_loads) {
        bind_textures(s, &cfg);
        memo.tex_bp = s->bp_gen;
        memo.tex_frame = frame;
        memo.tex_tluts = tlut_loads;
        memo.tex_ok = true;
    }
    ps3_prof_lap(PROF_DRAW_TEX, &t);
    ps3_diag.gp_site = "draw: RSX commands";
    for (i = 0; i < 16; i++) {
        if ((l->attrs >> i) & 1) {
            dev_attrib((int) i, io + l->attr[i].offset, l->out_size, l->attr[i].count, l->attr[i].type);
        } else {
            dev_attrib((int) i, 0, 0, 0, VTX_F32);
        }
    }
    if (trace_left != 0) {
        trace_left--;
        ps3_log("backend: draw %u prim %d vat %d count %u attrs %#x size %u vp %u fp %u\n", stats.draws, prim,
                vat, count, l->attrs, l->out_size, v->n, f->offset);
    }
    r = record_draw("GX draw", prim_type(prim), count, &efb_target);
    r->attrs = l->attrs;
    r->out_size = l->out_size;
    for (i = 0; i < 16; i++) {
        r->attr[i][0] = l->attr[i].offset;
        r->attr[i][1] = l->attr[i].count;
        r->attr[i][2] = l->attr[i].type;
    }
    r->vp = v;
    r->fp = f;
    for (i = 0; i < 8; i++) {
        if (bound[i] != NULL) {
            r->tex_on |= 1u << i;
            r->tex[i] = tex_memo[i].d;
        }
    }
    dev_draw(prim_type(prim), count);
    dev_mark(r->seq);
    ps3_prof.ticks[PROF_DRAW] += ps3_prof_now() - t0;
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

static const FpEntry* copy_program(const float colmat[28])
{
    static struct {
        float m[28];
        FpEntry e;
        bool used;
    } cache[16];
    static int next;
    int i;

    for (i = 0; i < 16; i++) {
        if (cache[i].used && memcmp(cache[i].m, colmat, sizeof(cache[i].m)) == 0) {
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
    if (fp_build(&cache[i].e, copy_gen(colmat, &fp_scratch)) == NULL) {
        return NULL;
    }
    cache[i].used = true;
    return &cache[i].e;
}

// Draws the src rectangle of the EFB into all of dst (dw x dh).
static void copy_pass(const DevTarget* dst, u32 sx, u32 sy, u32 sw, u32 sh, const FpEntry* fp, bool linear)
{
    static const float clip[4][2] = { { -1, 1 }, { 1, 1 }, { 1, -1 }, { -1, -1 } };
    float scale[4] = { dst->width * 0.5f, dst->height * -0.5f, 0.5f, 0.0f };
    float offset[4] = { dst->width * 0.5f, dst->height * 0.5f, 0.5f, 0.0f };
    float u0 = (float) sx / EFB_WIDTH, u1 = (float) (sx + sw) / EFB_WIDTH;
    float v0 = (float) sy / EFB_HEIGHT, v1 = (float) (sy + sh) / EFB_HEIGHT;
    const float tc[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
    CopyVertex* q;
    DrawRecord* r;
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
    t.offset = efb_target.color_offset;
    t.pitch = EFB_PITCH;
    t.width = EFB_WIDTH;
    t.height = EFB_HEIGHT;
    t.levels = 1;
    t.wrap_s = t.wrap_t = DEV_WRAP_CLAMP;
    t.min_filter = t.mag_filter = linear ? DEV_FILTER_LINEAR : DEV_FILTER_NEAREST;
    dev_texture(0, &t);
    bound[0] = NULL;
    use_vp(&copy_vp);
    use_fp(fp);
    for (i = 0; i < 16; i++) {
        dev_attrib(i, 0, 0, 0, VTX_F32);
    }
    dev_attrib(VA_POS, io, sizeof(CopyVertex), 4, VTX_F32);
    dev_attrib(VA_TEX0, io + 16, sizeof(CopyVertex), 2, VTX_F32);
    r = record_draw("EFB copy", DEV_PRIM_QUADS, 4, dst);
    r->attrs = (1u << VA_POS) | (1u << VA_TEX0);
    r->out_size = sizeof(CopyVertex);
    r->attr[VA_POS][1] = 4;
    r->attr[VA_TEX0][0] = 16;
    r->attr[VA_TEX0][1] = 2;
    r->vp = &copy_vp;
    r->fp = fp;
    r->tex_on = 1;
    r->tex[0] = t;
    dev_draw(DEV_PRIM_QUADS, 4);
    dev_mark(r->seq);
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

static void efb_copy(GpState* s)
{
    u32 v = s->bp[BP_EFB_COPY], tl = s->bp[0x49], wh = s->bp[0x4A];
    u32 sx = tl & 0x3FF, sy = (tl >> 10) & 0x3FF, sw = (wh & 0x3FF) + 1, sh = ((wh >> 10) & 0x3FF) + 1;
    u32 addr = (s->bp[0x4B] & 0xFFFFFF) << 5;
    uint64_t t = ps3_prof_now();

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
            const FpEntry* fp = copy_program(ident);

            x->width = (u16) sw;
            x->height = (u16) lines;
            if (fp != NULL) {
                copy_pass(&t, sx, sy, sw, sh, fp, lines != sh);
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

        if ((s->bp[0x43] & 7) == 3) {
            unsupported(U_DEPTH_COPY, "a depth copy");
        } else if (!copy_colmat(fmt, intensity, colmat)) {
            unsupported(U_COPY_FORMAT, "an EFB copy format");
        } else {
            const FpEntry* fp = copy_program(colmat);

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
                }
            }
            if (e != NULL && e->used && fp != NULL) {
                DevTarget t = { e->offset, e->pitch, 0, 0, (u16) dw, (u16) dh };

                e->frame = frame;
                copy_pass(&t, sx, sy, sw, sh, fp, half);
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
    textures_bound = false;
    memo.state_ok = false;
    memset(tex_memo, 0, sizeof(tex_memo));
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
        dev_local_free(e->mem);
        e->used = false;
    }
    if (waited) {
        memset(bound, 0, sizeof(bound));
        memset(tex_memo, 0, sizeof(tex_memo));
        textures_bound = false;
    }
}

static void finish(GpState* s)
{
    (void) s;
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

static const GpBackend backend = { draw, efb_copy, tlut_load, tex_invalidate, finish, present, mem_changed };

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
    initialized = true;
    return true;
}

void gx_rsx_present(u32 xfb)
{
    uint64_t t = ps3_prof_now();
    int i;

    for (i = 0; i < XFBS; i++) {
        if (xfbs[i].mem != NULL && xfbs[i].addr == xfb) {
            dev_barrier();
#ifdef PS3_DUMP_FRAMES
            if ((stats.presents + 1) % PS3_DUMP_FRAMES == 0 && stats.presents < 6 * PS3_DUMP_FRAMES) {
                dev_debug_dump("xfb", stats.presents + 1, xfbs[i].mem, EFB_PITCH, xfbs[i].width, xfbs[i].height);
            }
#endif
            dev_present(xfbs[i].offset, EFB_PITCH, xfbs[i].width, xfbs[i].height);
            dev_mark(record_draw("present", DEV_PRIM_QUADS, 4, &(DevTarget){ xfbs[i].offset })->seq);
            stats.presents++;
            // The present pass leaves its own state on the RSX.
            efb_bound = false;
            fp_current = NULL;
            vp_current = NULL;
            textures_bound = false;
            vc_valid = false;
            memo.state_ok = false;
            ps3_prof_lap(PROF_PRESENT, &t);
            return;
        }
    }
}

void gx_rsx_get_stats(GxRsxStats* out)
{
    *out = stats;
}

void gx_rsx_debug_hang(void)
{
    u32 done = dev_marked(), i;
    const DrawRecord* r = &records[(done + 1) % 16];

    if (r->seq != done + 1) {
        ps3_log("backend: the RSX finished draw %u of %u; the next one is not kept\n", done, draw_seq);
        return;
    }
    ps3_log("backend: the RSX finished draw %u of %u; draw %u is a %s: prim %u, %u vertices of %u bytes, "
            "attributes %#x, target %08x %ux%u\n",
            done, draw_seq, r->seq, r->kind, r->prim, r->count, r->out_size, r->attrs, r->target, r->target_w,
            r->target_h);
    for (i = 0; i < 16; i++) {
        if ((r->attrs >> i) & 1) {
            ps3_log("backend:  attribute %u: offset %u, %u of type %u\n", i, r->attr[i][0], r->attr[i][1],
                    r->attr[i][2]);
        }
    }
    for (i = 0; i < 8; i++) {
        const DevTexture* t = &r->tex[i];

        if ((r->tex_on >> i) & 1) {
            ps3_log("backend:  texture %u: %08x %ux%u, pitch %u, %u levels, swizzled %u, wrap %u %u, filter %u %u, "
                    "lod %u to %u, bias %d\n",
                    i, t->offset, t->width, t->height, t->pitch, t->levels, t->swizzled, t->wrap_s, t->wrap_t,
                    t->min_filter, t->mag_filter, t->min_lod, t->max_lod, t->lod_bias);
        }
    }
    if (r->fp != NULL) {
        const TevConfig* c = &r->fp->cfg;
        // Local memory: slow to read on the PPE, and the program stays where the draw used it.
        const volatile u32* w = r->fp->mem;

        ps3_log("backend:  fragment program at %08x: %u words, %u registers, texcoords %#x (%#x with 4), TEV %u "
                "stages, fog %u, konst inputs %d %d %d %d, register inputs %d %d %d %d\n",
                r->fp->offset, r->fp->words, r->fp->regs, r->fp->texcoords, r->fp->texcoords4, c->nstages, c->fog,
                c->kin[0], c->kin[1], c->kin[2], c->kin[3], c->rin[0], c->rin[1], c->rin[2], c->rin[3]);
        for (i = 0; i < c->nstages; i++) {
            const TevStage* s = &c->st[i];

            ps3_log("backend:  stage %u: tex %d coord %u proj %u ras %u konst %u %u; colour %u %u %u %u op %u "
                    "bias %u scale %u clamp %u dest %u; alpha %u %u %u %u op %u bias %u scale %u clamp %u dest %u\n",
                    i, s->tex, s->texcoord, s->proj, s->ras, s->kcsel, s->kasel, s->col.a, s->col.b, s->col.c,
                    s->col.d, s->col.op, s->col.bias, s->col.scale, s->col.clamp, s->col.dest, s->alp.a, s->alp.b,
                    s->alp.c, s->alp.d, s->alp.op, s->alp.bias, s->alp.scale, s->alp.clamp, s->alp.dest);
        }
        for (i = 0; i < r->fp->words; i += 8) {
            ps3_log("backend:  fp %3u: %08x %08x %08x %08x %08x %08x %08x %08x\n", i, w[i], w[i + 1], w[i + 2],
                    w[i + 3], w[i + 4], w[i + 5], w[i + 6], w[i + 7]);
        }
    }
    if (r->vp != NULL) {
        ps3_log("backend:  vertex program: %u instructions, inputs %#x, outputs %#x, %u registers\n", r->vp->n,
                r->vp->inputs, r->vp->outputs, r->vp->regs);
        for (i = 0; i < r->vp->n; i++) {
            ps3_log("backend:  vp %3u: %08x %08x %08x %08x\n", i, r->vp->ucode[i][0], r->vp->ucode[i][1],
                    r->vp->ucode[i][2], r->vp->ucode[i][3]);
        }
    }
}
