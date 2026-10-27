// PC check of the RSX program generators against cgcomp: "rsxcheck gen DIR" writes each case's assembly (.asm) and
// ucode (.bin, big-endian); "rsxcheck cmp DIR" compares each .bin with the ucode in cgcomp's .fpo or .vpo.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../gx/tevgen.h"
#include "../../gx/vpgen.h"
#ifndef NO_XF
#include "../../gx/xfgen.h"
#endif

#define MAX_CASES 128

typedef struct Case {
    char name[48];
    char kind; // 'F' fragment, 'V' vertex
    int words;
    u32 uc[8192];
    char* text;
} Case;

static Case cases[MAX_CASES];
static int ncases;
static FpProg fp;
static VpProg vp;

static TevComb comb(u8 a, u8 b, u8 c, u8 d)
{
    TevComb t = { a, b, c, d, 0, TB_ZERO, TS_1, 1, 0 };
    return t;
}

static TevComb comb_x(u8 a, u8 b, u8 c, u8 d, u8 op, u8 bias, u8 scale, u8 clamp, u8 dest)
{
    TevComb t = { a, b, c, d, op, bias, scale, clamp, dest };
    return t;
}

static TevConfig* new_tev(TevConfig* t)
{
    static const float r[4][4] = {
        { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.9f, 0.5f, 0.1f, 0.8f }, { 0.25f, 0.75f, 0.5f, 0.3f }, { 0.6f, 0.6f, 0.2f, 0.9f }
    };
    int i;

    memset(t, 0, sizeof(*t));
    memcpy(t->reg, r, sizeof(r));
    for (i = 0; i < 4; i++) {
        t->kin[i] = -1;
        t->rin[i] = -1;
        t->kval[i][0] = 0.1f * i;
        t->kval[i][1] = 0.2f + 0.1f * i;
        t->kval[i][2] = 0.5f;
        t->kval[i][3] = 0.9f - 0.2f * i;
    }
    t->fog_pin = -1;
    t->fog_cin = -1;
    return t;
}

static TevStage* stage(TevConfig* t, TevComb col, TevComb alp, int tex, int tc)
{
    TevStage* s = &t->st[t->nstages++];
    int i;

    s->col = col;
    s->alp = alp;
    s->tex = (s8) tex;
    s->texcoord = (u8) tc;
    s->ras = RAS_COL0;
    for (i = 0; i < 4; i++) {
        s->ras_swap[i] = s->tex_swap[i] = (u8) i;
    }
    s->kcsel = 0x0C;
    s->kasel = 0x1C;
    return s;
}

static void add_fp(const char* name, const TevConfig* t)
{
    Case* c = &cases[ncases++];

    snprintf(c->name, sizeof(c->name), "%s", name);
    c->kind = 'F';
    if (tev_gen(t, &fp) < 0) {
        printf("%s: generation failed (error %d)\n", name, fp.err);
        exit(1);
    }
    c->words = fp_ucode(&fp, c->uc, 8192);
    c->text = strdup(fp.text);
}

static void add_vp(const char* name)
{
    Case* c = &cases[ncases++];
    int i, j;

    snprintf(c->name, sizeof(c->name), "%s", name);
    c->kind = 'V';
    vp_end(&vp);
    if (vp.err) {
        printf("%s: generation failed (error %d)\n", name, vp.err);
        exit(1);
    }
    c->words = vp.n * 4;
    for (i = 0; i < vp.n; i++) {
        for (j = 0; j < 4; j++) {
            c->uc[i * 4 + j] = vp.w[i][j];
        }
        // The check text has c[n] for c[A0 + n]: drop the index flag and the address swizzle.
        if (vp.rel[i]) {
            c->uc[i * 4] &= ~3u;
            c->uc[i * 4 + 3] &= ~2u;
        }
    }
    c->text = strdup(vp.text);
}

static void fp_cases(void)
{
    TevConfig t;
    TevStage* s;

    new_tev(&t);
    stage(&t, comb(CC_ZERO, CC_ZERO, CC_ZERO, CC_RASC), comb(CA_ZERO, CA_ZERO, CA_ZERO, CA_RASA), -1, 0);
    add_fp("passcolor", &t);

    new_tev(&t);
    stage(&t, comb(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO), comb(CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO), 0, 0);
    add_fp("modulate", &t);

    new_tev(&t);
    s = stage(&t, comb(CC_C0, CC_TEXC, CC_RASA, CC_ZERO), comb(CA_ZERO, CA_TEXA, CA_KONST, CA_A1), 1, 1);
    s->kasel = 0x05;
    s = stage(&t, comb_x(CC_ZERO, CC_CPREV, CC_KONST, CC_C2, 1, TB_ADDHALF, TS_2, 1, 1),
              comb_x(CA_ZERO, CA_APREV, CA_A0, CA_ZERO, 0, TB_SUBHALF, TS_HALF, 0, 2), 2, 0);
    s->kcsel = 0x15;
    s = stage(&t, comb(CC_C1, CC_APREV, CC_HALF, CC_ONE), comb(CA_A1, CA_A2, CA_APREV, CA_ZERO), -1, 0);
    s->ras = RAS_COL1;
    add_fp("three stages", &t);

    // The konst colours and start values from interpolants.
    new_tev(&t);
    t.kin[0] = FIN_TC0 + 6;
    t.kin[2] = FIN_TC0 + 7;
    t.rin[1] = FIN_TC0 + 5;
    s = stage(&t, comb(CC_C0, CC_KONST, CC_TEXA, CC_ZERO), comb(CA_ZERO, CA_KONST, CA_TEXA, CA_A0), 0, 0);
    s->kcsel = 0x0C;
    s->kasel = 0x1C;
    s = stage(&t, comb(CC_CPREV, CC_KONST, CC_RASC, CC_ZERO), comb(CA_ZERO, CA_KONST, CA_RASA, CA_ZERO), -1, 0);
    s->kcsel = 0x12;
    s->kasel = 0x1A;
    add_fp("konst inputs", &t);

    new_tev(&t);
    s = stage(&t, comb(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO), comb(CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO), 0, 0);
    s->proj = 1;
    s->tex_swap[0] = 3;
    s->ras_swap[2] = 0;
    add_fp("projected", &t);

    new_tev(&t);
    stage(&t, comb(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO), comb(CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO), 0, 0);
    t.fog = FOG_LINEAR;
    t.fog_pin = FIN_TC0 + 7;
    t.fog_cin = FIN_TC0 + 6;
    add_fp("fog linear", &t);
    t.fog = FOG_EXP2;
    add_fp("fog exp2", &t);
    t.fog = FOG_REV_EXP;
    t.fog_ortho = 1;
    add_fp("fog rev exp ortho", &t);
    t.fog = FOG_EXP;
    t.fog_ortho = 0;
    t.fog_pin = -1;
    t.fog_cin = -1;
    t.fog_param[0] = 0.5f;
    t.fog_param[1] = 8388608.0f;
    t.fog_param[2] = 0.25f;
    t.fog_param[3] = 0.125f;
    add_fp("fog inline", &t);

    {
        float colmat[28];
        int i;

        for (i = 0; i < 28; i++) {
            colmat[i] = (float) (i % 5) * 0.25f;
        }
        copy_gen(colmat, &fp);
        cases[ncases].kind = 'F';
        snprintf(cases[ncases].name, sizeof(cases[ncases].name), "copy");
        cases[ncases].words = fp_ucode(&fp, cases[ncases].uc, 8192);
        cases[ncases].text = strdup(fp.text);
        ncases++;
    }
}

static void vp_cases(void)
{
    VpSrc z = vp_t(0, NULL);

    vp_init(&vp);
    vp_op(&vp, VOP_MOV, VP_OUT(VO_POS), 0xF, false, vp_in(0, NULL), z, z);
    vp_op(&vp, VOP_MOV, VP_OUT(VO_COL0), 0xF, false, vp_in(3, NULL), z, z);
    vp_op(&vp, VOP_MOV, VP_OUT(VO_TC0 + 2), 0x3, false, vp_in(8, NULL), z, z);
    add_vp("pass");

    vp_init(&vp);
    vp_op(&vp, VOP_DP4, 1, 1, false, vp_c(4, NULL), vp_in(0, NULL), z);
    vp_op(&vp, VOP_DP4, 1, 2, false, vp_c(5, NULL), vp_in(0, NULL), z);
    vp_op(&vp, VOP_DP3, 1, 4, false, vp_c(6, "xyzx"), vp_t(2, "zyxw"), z);
    vp_op(&vp, VOP_MAD, 2, 0xF, true, vp_t(1, "xxyy"), vp_c(7, NULL), vp_neg(vp_t(3, NULL)));
    vp_op(&vp, VOP_ADD, 3, 0x7, false, vp_t(2, NULL), vp_neg(vp_in(2, "zxyw")), z);
    vp_op(&vp, VOP_MUL, VP_OUT(VO_POS), 0xF, false, vp_t(3, NULL), vp_c(8, "wwww"), z);
    vp_op(&vp, VOP_MIN, 4, 0x9, false, vp_t(3, NULL), vp_c(9, NULL), z);
    vp_op(&vp, VOP_MAX, VP_OUT(VO_COL1), 0xF, true, vp_t(4, NULL), vp_t(1, NULL), z);
    vp_op(&vp, VOP_SLT, 5, 0xF, false, vp_t(4, NULL), vp_c(10, NULL), z);
    vp_op(&vp, VOP_SGE, 5, 0x2, false, vp_t(4, "y"), vp_t(5, "x"), z);
    vp_op(&vp, VOP_FRC, 6, 0xF, false, vp_t(5, NULL), z, z);
    vp_op(&vp, VOP_FLR, VP_OUT(VO_TC0), 0xF, false, vp_t(6, NULL), z, z);
    add_vp("vector ops");

    vp_init(&vp);
    vp_op(&vp, VOP_DP3, 1, 1, false, vp_in(2, NULL), vp_in(2, NULL), z);
    vp_op(&vp, VOP_RSQ, 1, 2, false, vp_t(1, "x"), z, z);
    vp_op(&vp, VOP_RCP, 2, 1, false, vp_t(1, "y"), z, z);
    vp_op(&vp, VOP_LG2, 2, 4, false, vp_t(2, "x"), z, z);
    vp_op(&vp, VOP_EX2, VP_OUT(VO_FOGC), 1, false, vp_t(2, "z"), z, z);
    vp_op(&vp, VOP_RCP, VP_OUT(VO_TC0 + 7), 8, false, vp_c(3, "w"), z, z);
    vp_op(&vp, VOP_MOV, VP_OUT(VO_POS), 0xF, false, vp_t(2, NULL), z, z);
    add_vp("scalar ops");

    vp_init(&vp);
    vp_op(&vp, VOP_ARL, VP_A0, 1, false, vp_in(14, "x"), z, z);
    vp_op(&vp, VOP_MOV, VP_OUT(VO_POS), 0xF, false, vp_in(0, NULL), z, z);
    add_vp("arl");
}

static void write_be(FILE* f, const u32* w, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        u8 b[4] = { (u8) (w[i] >> 24), (u8) (w[i] >> 16), (u8) (w[i] >> 8), (u8) w[i] };
        fwrite(b, 1, 4, f);
    }
}

static long slurp(const char* path, u8** out)
{
    FILE* f = fopen(path, "rb");
    long n;

    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = malloc(n);
    if (fread(*out, 1, n, f) != (size_t) n) {
        n = -1;
    }
    fclose(f);
    return n;
}

static u32 be32(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

int main(int argc, char** argv)
{
    char path[256];
    int i, bad = 0;

    if (argc < 3) {
        printf("usage: rsxcheck gen|cmp DIR\n");
        return 2;
    }
    fp_cases();
    vp_cases();
#ifndef NO_XF
    xf_check_cases(&vp, add_vp);
#endif
    for (i = 0; i < ncases; i++) {
        Case* c = &cases[i];
        const char* ext = c->kind == 'F' ? "fpo" : "vpo";

        if (!strcmp(argv[1], "gen")) {
            FILE* f;

            snprintf(path, sizeof(path), "%s/case%02d.%casm", argv[2], i, c->kind == 'F' ? 'f' : 'v');
            f = fopen(path, "w");
            fputs(c->text, f);
            fclose(f);
            snprintf(path, sizeof(path), "%s/case%02d.bin", argv[2], i);
            f = fopen(path, "wb");
            write_be(f, c->uc, c->words);
            fclose(f);
        } else {
            u8* obj;
            long n;
            u32 uoff, k;

            snprintf(path, sizeof(path), "%s/case%02d.%s", argv[2], i, ext);
            n = slurp(path, &obj);
            if (n < 0) {
                printf("%-28s missing .%s\n", c->name, ext);
                bad++;
                continue;
            }
            uoff = be32(obj + 20);
            for (k = 0; k < (u32) c->words; k++) {
                if (uoff + 4 * k + 4 > (u32) n || be32(obj + uoff + 4 * k) != c->uc[k]) {
                    break;
                }
            }
            if (k == (u32) c->words && uoff + 4 * k == (u32) n) {
                printf("%-28s %c %4d words: match\n", c->name, c->kind, c->words);
            } else {
                printf("%-28s %c %4d words: DIFFER at word %u (cgcomp %08X, gen %08X), cgcomp %ld bytes\n", c->name,
                       c->kind, c->words, k, uoff + 4 * k + 4 <= (u32) n ? be32(obj + uoff + 4 * k) : 0,
                       k < (u32) c->words ? c->uc[k] : 0, n - (long) uoff);
                bad++;
            }
            free(obj);
        }
    }
    if (!strcmp(argv[1], "cmp")) {
        printf("%d of %d cases match\n", ncases - bad, ncases);
    }
    return bad != 0;
}
