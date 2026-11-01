// ps3_sinf/ps3_cosf against newlib's libm.a, the matrix and FObj code against math_ref.c, bit for bit, all built by
// ppu-gcc (Makefile mathobjs). math_test trig LO HI (float bit patterns, hex) | mtx CASES SEED | fobj
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef float f32;
typedef f32 Mtx[3][4];
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

float sinf(float);
float cosf(float);
float ps3_sinf(float);
float ps3_cosf(float);
void PSMTXConcat(Mtx a, Mtx b, Mtx ab);
void HSD_MtxInverseTranspose(Mtx src, Mtx dest);
void HSD_MtxScaledAdd(Mtx m, Mtx in, Mtx out, f32 w);
void ref_PSMTXConcat(Mtx a, Mtx b, Mtx ab);
void ref_HSD_MtxInverseTranspose(Mtx src, Mtx dest);
void ref_HSD_MtxScaledAdd(Mtx m, Mtx in, Mtx out, f32 w);
extern f32 (*math_parseFloat)(u8** pos, u8 frac);
f32 ref_parseFloat(u8** pos, u8 frac);
f32 ref_fterm_recip(u16 fterm);
f32 new_fterm_recip(f32 fterm_f);

// newlib's libm sets errno through __errno; the objects' allocator and assert are never reached here.
int* __errno(void)
{
    static int e;
    return &e;
}
void* HSD_ObjAlloc(void* data) { abort(); }
void HSD_ObjFree(void* data, void* obj) { abort(); }
void HSD_ObjAllocInit(void* data, unsigned long size, unsigned long align) { abort(); }
void __assert(const char* file, int line, const char* cond) { abort(); }
f32 splGetHelmite(f32 fterm, f32 time, f32 p0, f32 p1, f32 d0, f32 d1) { abort(); }

static u64 bad;
static u64 seed = 1;

static u32 rnd(void)
{
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return (u32) (seed >> 32);
}

static u32 f2u(f32 f)
{
    u32 u;

    memcpy(&u, &f, 4);
    return u;
}

static f32 u2f(u32 u)
{
    f32 f;

    memcpy(&f, &u, 4);
    return f;
}

static void report(const char* what, u64 n, u32 a, u32 b)
{
    if (bad++ < 20) {
        printf("%s case %llu: %08x, reference %08x\n", what, n, a, b);
    }
}

static void trig(u64 lo, u64 hi)
{
    u64 i;

    for (i = lo; i < hi; i++) {
        f32 x = u2f((u32) i);
        u32 s = f2u(ps3_sinf(x)), rs = f2u(sinf(x));
        u32 c = f2u(ps3_cosf(x)), rc = f2u(cosf(x));

        if (s != rs) {
            report("sinf", i, s, rs);
        }
        if (c != rc) {
            report("cosf", i, c, rc);
        }
    }
    printf("trig %llx-%llx: %llu inputs\n", lo, hi, hi - lo);
}

// Matrix elements: ordinary values, then +-0, +-Inf, NaNs with random payloads and signs, denormals, any bits.
static f32 element(void)
{
    u32 r = rnd() % 100;

    if (r < 45) {
        return (f32) ((int) (rnd() % 2000001) - 1000000) / 250000.0f;
    }
    if (r < 50) {
        return u2f(rnd() & 0x80000000);
    }
    if (r < 55) {
        return u2f(0x7F800000 | (rnd() & 0x80000000));
    }
    if (r < 75) {
        return u2f(0x7F800000 | (rnd() & 0x807FFFFF) | 1);
    }
    if (r < 85) {
        return u2f(rnd() & 0x807FFFFF);
    }
    return u2f(rnd());
}

static void fill(Mtx m)
{
    int i;

    for (i = 0; i < 12; i++) {
        (&m[0][0])[i] = element();
    }
}

static void same(const char* what, u64 n, Mtx a, Mtx b)
{
    int i;

    for (i = 0; i < 12; i++) {
        u32 x = f2u((&a[0][0])[i]), y = f2u((&b[0][0])[i]);

        if (x != y) {
            report(what, n, x, y);
            return;
        }
    }
}

static void mtx(u64 cases)
{
    u64 n;

    for (n = 0; n < cases; n++) {
        Mtx a, b, o1, o2, x1, x2;
        f32 w = element();

        fill(a);
        fill(b);
        if (rnd() % 4 == 0) {
            // A small determinant: the copy path of the inverse transpose.
            memset(a, 0, sizeof(Mtx));
            a[0][0] = element();
        }
        PSMTXConcat(a, b, o1);
        ref_PSMTXConcat(a, b, o2);
        same("concat", n, o1, o2);
        memcpy(x1, a, sizeof(Mtx));
        memcpy(x2, a, sizeof(Mtx));
        PSMTXConcat(x1, b, x1);
        ref_PSMTXConcat(x2, b, x2);
        same("concat ab=a", n, x1, x2);
        memcpy(x1, b, sizeof(Mtx));
        memcpy(x2, b, sizeof(Mtx));
        PSMTXConcat(a, x1, x1);
        ref_PSMTXConcat(a, x2, x2);
        same("concat ab=b", n, x1, x2);
        memcpy(x1, a, sizeof(Mtx));
        memcpy(x2, a, sizeof(Mtx));
        PSMTXConcat(x1, x1, x1);
        ref_PSMTXConcat(x2, x2, x2);
        same("concat a=b=ab", n, x1, x2);

        HSD_MtxInverseTranspose(a, o1);
        ref_HSD_MtxInverseTranspose(a, o2);
        same("invt", n, o1, o2);
        memcpy(x1, a, sizeof(Mtx));
        memcpy(x2, a, sizeof(Mtx));
        HSD_MtxInverseTranspose(x1, x1);
        ref_HSD_MtxInverseTranspose(x2, x2);
        same("invt src=dest", n, x1, x2);

        HSD_MtxScaledAdd(a, b, o1, w);
        ref_HSD_MtxScaledAdd(a, b, o2, w);
        same("scaledadd", n, o1, o2);
        memcpy(x1, b, sizeof(Mtx));
        memcpy(x2, b, sizeof(Mtx));
        HSD_MtxScaledAdd(a, x1, x1, w);
        ref_HSD_MtxScaledAdd(a, x2, x2, w);
        same("scaledadd out=in", n, x1, x2);
        memcpy(x1, a, sizeof(Mtx));
        memcpy(x2, a, sizeof(Mtx));
        HSD_MtxScaledAdd(x1, b, x1, w);
        ref_HSD_MtxScaledAdd(x2, b, x2, w);
        same("scaledadd out=m", n, x1, x2);
    }
    printf("mtx: %llu cases\n", cases);
}

static void parse_one(u32 frac, const u8* d, u64 n)
{
    u8* p1 = (u8*) d;
    u8* p2 = (u8*) d;
    u32 a = f2u(math_parseFloat(&p1, (u8) frac)), b = f2u(ref_parseFloat(&p2, (u8) frac));

    if (a != b || p1 != p2) {
        report("parseFloat", n, a, b);
    }
}

static void fobj(void)
{
    u8 d[4];
    u32 frac, i;

    for (frac = 0; frac < 256; frac++) {
        for (i = 0; i < 65536; i++) {
            d[0] = i & 0xFF;
            d[1] = i >> 8;
            d[2] = rnd();
            d[3] = rnd();
            parse_one(frac, d, frac << 16 | i);
        }
    }
    for (i = 0; i < 16777216; i++) {
        u32 v = rnd();

        memcpy(d, &v, 4);
        parse_one(0, d, i);
    }
    for (i = 1; i < 65536; i++) {
        u32 a = f2u(new_fterm_recip((f32) i)), b = f2u(ref_fterm_recip((u16) i));

        if (a != b) {
            report("1/fterm", i, a, b);
        }
    }
    printf("fobj: parseFloat 256 x 65536 + 2^24 floats, 1/fterm 1..65535\n");
}

int main(int argc, char** argv)
{
    if (argc == 4 && strcmp(argv[1], "trig") == 0) {
        trig(strtoull(argv[2], NULL, 16), strtoull(argv[3], NULL, 16));
    } else if (argc == 4 && strcmp(argv[1], "mtx") == 0) {
        seed = strtoull(argv[3], NULL, 0);
        mtx(strtoull(argv[2], NULL, 0));
    } else if (argc == 2 && strcmp(argv[1], "fobj") == 0) {
        fobj();
    } else {
        printf("usage: math_test trig LO HI | mtx CASES SEED | fobj\n");
        return 2;
    }
    printf("%s: %llu mismatches\n", bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
