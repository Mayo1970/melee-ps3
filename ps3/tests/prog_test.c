// PC test of the generated programs: random XF states run through xf_gen's program and xf_ref, random TEV states
// through tev_gen's program and tev_ref, on the ucode interpreters. Prints the mismatches.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gx/tevgen.h"
#include "../gx/xfgen.h"
#include "rsx_interp.h"

#define XF_CASES 3000
#define TEV_CASES 3000

static u32 seed = 1;
static VpProg vp;
static FpProg fp;
static u32 ucode[8192];

static u32 rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float) (rnd() & 0xFFFF) / 65535.0f;
}

static bool close(float a, float b, float tol)
{
    float m = fabsf(b) > 1.0f ? fabsf(b) : 1.0f;
    return fabsf(a - b) <= tol * m;
}

static int xf_fail, tev_fail;

static void xf_case(int n)
{
    static float c[C_COUNT][4];
    XfKey key;
    XfIn in;
    XfOut ref;
    float out[16][4];
    int i, k, nconst = 0, slot;

    memset(&key, 0, sizeof(key));
    memset(&in, 0, sizeof(in));
    key.attrs = 1u << VA_POS;
    for (i = 1; i < 12; i++) {
        if (rnd() & 1) {
            key.attrs |= 1u << i;
        }
    }
    key.pos3 = rnd() & 1;
    // Every other case, outside rnd(): the random states stay those of the earlier runs.
    key.old_depth = (u8) (n & 1);
    key.numchans = rnd() % 3;
    key.numtexgens = rnd() % 5;
    key.dualtex = rnd() & 1;
    for (i = 0; i < 4; i++) {
        key.chan[i] = rnd() & 0x7FFF;
    }
    for (i = 0; i < key.numtexgens; i++) {
        static const u8 rows[] = { 0, 1, 3, 5, 6, 7 };
        static const u8 types[] = { 0, 0, 0, 2, 3 };

        key.texinfo[i] = (rnd() & 6) | (u32) types[rnd() % 5] << 4 | (u32) rows[rnd() % 6] << 7;
        key.postinfo[i] = key.dualtex ? (rnd() % 62) | (rnd() & 1) << 8 : 0;
    }
    key.tev_first = key.numtexgens;
    for (i = 0; i < XF_TEV_CONSTS && key.tev_first + nconst < 8; i++) {
        if (rnd() & 1) {
            key.tev_consts |= 1u << i;
            nconst++;
        }
    }
    for (i = 0; i < C_COUNT; i++) {
        for (k = 0; k < 4; k++) {
            c[i][k] = frand(-1.0f, 1.0f);
        }
    }
    for (i = 0; i < 8; i++) {
        float* l = c[C_LIGHT + LIGHT_STRIDE * i];
        float len;

        for (k = 0; k < 4; k++) {
            l[k] = frand(0.0f, 1.0f);
        }
        l[4] = frand(0.0f, 1.0f);
        l[5] = frand(0.0f, 1.0f);
        l[6] = frand(0.0f, 1.0f);
        l[8] = frand(0.5f, 1.5f);
        l[9] = frand(0.0f, 0.5f);
        l[10] = frand(0.0f, 0.5f);
        for (k = 0; k < 3; k++) {
            l[12 + k] = frand(-10.0f, 10.0f);
        }
        len = sqrtf(l[16] * l[16] + l[17] * l[17] + l[18] * l[18]);
        for (k = 0; k < 3; k++) {
            l[16 + k] /= len;
        }
    }
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 4; k++) {
            c[C_CHAN + i][k] = frand(0.0f, 1.0f);
        }
    }
    for (i = 0; i < 3; i++) {
        for (k = 0; k < 4; k++) {
            c[C_DQ + i][k] = 1.0f / (float) (1 << (rnd() % 8));
        }
    }
    c[C_MIDX][0] = (float) (3 * (rnd() % 10));
    for (k = 1; k < 4; k++) {
        c[C_MIDX][k] = (float) (30 + 3 * (rnd() % 10));
    }
    for (k = 0; k < 4; k++) {
        c[C_MIDX + 1][k] = (float) (30 + 3 * (rnd() % 10));
    }
    c[C_MIDX + 2][0] = (float) (30 + 3 * (rnd() % 10));
    c[C_MISC][0] = 0.0f;
    c[C_MISC][1] = 1.0f;
    c[C_MISC][2] = 0.5f;
    c[C_MISC][3] = 2.0f;
    for (i = 0; i < 16; i++) {
        for (k = 0; k < 4; k++) {
            in.a[i][k] = frand(-5.0f, 5.0f);
        }
        in.a[i][3] = 1.0f;
    }
    for (k = 0; k < 4; k++) {
        in.a[VA_MIDX_A][k] = (float) (k == 0 ? 3 * (rnd() % 10) : 30 + 3 * (rnd() % 10));
        in.a[VA_MIDX_B][k] = (float) (30 + 3 * (rnd() % 10));
        in.a[VA_COL0][k] = frand(0.0f, 1.0f);
        in.a[VA_COL1][k] = frand(0.0f, 1.0f);
    }
    // Absent attributes read as the RSX defaults.
    for (i = 0; i < 16; i++) {
        if (!(key.attrs & (1u << i))) {
            in.a[i][0] = in.a[i][1] = in.a[i][2] = 0.0f;
            in.a[i][3] = 1.0f;
        }
    }
    if (!key.pos3) {
        in.a[VA_POS][2] = 0.0f;
    }
    if (xf_gen(&key, &vp) < 0) {
        printf("xf case %d: generation failed (error %d)\n", n, vp.err);
        xf_fail++;
        return;
    }
    memset(out, 0, sizeof(out));
    vp_run((const u32(*)[4]) vp.w, vp.n, (const float(*)[4]) c, (const float(*)[4]) in.a, out);
    xf_ref(&key, (const float(*)[4]) c, &in, &ref);
    for (k = 0; k < 4; k++) {
        bool ok = close(out[0][k], ref.pos[k], 1e-4f) && close(out[1][k], ref.col[0][k], 1e-3f) &&
                  close(out[2][k], ref.col[1][k], 1e-3f);

        for (i = 0, slot = 0; i < 8; i++) {
            bool used = i < key.numtexgens || (i >= key.tev_first && i < key.tev_first + nconst);

            if (used && !close(out[7 + i][k], ref.tc[i][k], 1e-3f)) {
                ok = false;
            }
        }
        if (!ok) {
            if (xf_fail < 10) {
                printf("xf case %d component %d: pos %g/%g col0 %g/%g col1 %g/%g tc0 %g/%g (attrs %x chans %d "
                       "texgens %d)\n",
                       n, k, out[0][k], ref.pos[k], out[1][k], ref.col[0][k], out[2][k], ref.col[1][k], out[7][k],
                       ref.tc[0][k], key.attrs, key.numchans, key.numtexgens);
            }
            xf_fail++;
            return;
        }
    }
}

typedef struct Texels {
    float tex[8][4];
} Texels;

static void sample(void* user, int unit, const float coord[4], float out[4])
{
    memcpy(out, ((Texels*) user)->tex[unit], 16);
}

static void tev_case(int n)
{
    TevConfig cfg;
    Texels tx;
    float in[16][4], ras[2][4], out[4], ref[4], depth;
    int i, k, words, slot = 0;

    memset(&cfg, 0, sizeof(cfg));
    memset(in, 0, sizeof(in));
    cfg.nstages = 1 + rnd() % 4;
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 4; k++) {
            cfg.kval[i][k] = frand(0.0f, 1.0f);
            cfg.reg[i][k] = frand(-0.5f, 1.5f);
        }
        cfg.kin[i] = -1;
        cfg.rin[i] = -1;
        if ((rnd() & 1) && slot < 7) {
            cfg.kin[i] = (s8) (FIN_TC0 + slot);
            memcpy(in[FIN_TC0 + slot++], cfg.kval[i], 16);
        }
        if ((rnd() & 1) && slot < 7) {
            cfg.rin[i] = (s8) (FIN_TC0 + slot);
            memcpy(in[FIN_TC0 + slot++], cfg.reg[i], 16);
        }
    }
    for (i = 0; i < cfg.nstages; i++) {
        TevStage* st = &cfg.st[i];
        TevComb* cb[2] = { &st->col, &st->alp };
        int j;

        for (j = 0; j < 2; j++) {
            int nsel = j == 0 ? 16 : 8;

            cb[j]->a = (u8) (rnd() % nsel);
            cb[j]->b = (u8) (rnd() % nsel);
            cb[j]->c = (u8) (rnd() % nsel);
            cb[j]->d = (u8) (rnd() % nsel);
            cb[j]->op = rnd() & 1;
            cb[j]->bias = (u8) (rnd() % 3);
            cb[j]->scale = (u8) (rnd() % 4);
            cb[j]->clamp = rnd() & 1;
            cb[j]->dest = (u8) (rnd() % 4);
        }
        st->tex = (rnd() % 3) ? (s8) (rnd() % 8) : -1;
        st->texcoord = (u8) (rnd() % 2);
        st->ras = (u8) (rnd() % 3);
        for (k = 0; k < 4; k++) {
            st->ras_swap[k] = (u8) (rnd() % 4);
            st->tex_swap[k] = (u8) (rnd() % 4);
        }
        st->kcsel = (u8) (rnd() % 0x20);
        st->kasel = (u8) (rnd() % 0x20);
    }
    if (rnd() & 1) {
        static const u8 types[] = { FOG_LINEAR, FOG_EXP, FOG_EXP2, FOG_REV_EXP, FOG_REV_EXP2 };

        cfg.fog = types[rnd() % 5];
        cfg.fog_ortho = rnd() & 1;
        cfg.fog_param[0] = cfg.fog_ortho ? frand(0.5f, 2.0f) / 16777216.0f : frand(0.5f, 2.0f) * 16777216.0f;
        cfg.fog_param[1] = frand(1.6e7f, 3.3e7f);
        cfg.fog_param[2] = 0.5f;
        cfg.fog_param[3] = frand(0.0f, 0.5f);
        for (k = 0; k < 3; k++) {
            cfg.fog_color[k] = frand(0.0f, 1.0f);
        }
        cfg.fog_pin = cfg.fog_cin = -1;
        if ((rnd() & 1) && slot < 7) {
            cfg.fog_pin = (s8) (FIN_TC0 + slot);
            memcpy(in[FIN_TC0 + slot++], cfg.fog_param, 16);
        }
        if ((rnd() & 1) && slot < 7) {
            cfg.fog_cin = (s8) (FIN_TC0 + slot);
            memcpy(in[FIN_TC0 + slot++], cfg.fog_color, 16);
        }
    }
    for (i = 0; i < 2; i++) {
        for (k = 0; k < 4; k++) {
            ras[i][k] = frand(0.0f, 1.0f);
            in[FIN_COL0 + i][k] = ras[i][k];
        }
    }
    for (i = 0; i < 8; i++) {
        for (k = 0; k < 4; k++) {
            tx.tex[i][k] = frand(0.0f, 1.0f);
        }
    }
    depth = frand(0.0f, 0.9f);
    in[FIN_WPOS][2] = depth;
    if (tev_gen(&cfg, &fp) < 0) {
        printf("tev case %d: generation failed (error %d)\n", n, fp.err);
        tev_fail++;
        return;
    }
    words = fp_ucode(&fp, ucode, 8192);
    fp_run(ucode, words, (const float(*)[4]) in, sample, &tx, out);
    tev_ref(&cfg, (const float(*)[4]) ras, (const float(*)[4]) tx.tex, depth, ref);
    for (k = 0; k < 4; k++) {
        if (!close(out[k], ref[k], 1e-3f)) {
            if (tev_fail < 10) {
                printf("tev case %d component %d: %g, reference %g (%d stages, fog %d)\n", n, k, out[k], ref[k],
                       cfg.nstages, cfg.fog);
            }
            tev_fail++;
            return;
        }
    }
}

int main(void)
{
    int i;

    vp_set_text(false);
    fp_set_text(false);
    for (i = 0; i < XF_CASES; i++) {
        xf_case(i);
    }
    for (i = 0; i < TEV_CASES; i++) {
        tev_case(i);
    }
    printf("XF: %d of %d cases match; TEV: %d of %d cases match\n", XF_CASES - xf_fail, XF_CASES,
           TEV_CASES - tev_fail, TEV_CASES);
    return xf_fail || tev_fail;
}
