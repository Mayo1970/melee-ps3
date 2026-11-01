// newlib mathfp's sinf and cosf (sinef, which the PS3 links) op for op, bit for bit the same (tests/math_test.c).
// Built with -ffp-contract=off, so the only fused operations are the fmas sinef has. No ps3_trig.h: libm_* reach libm.
#include <math.h>

// sinef's constants (libm.a sf_sine.o; ROOTEPS is z_rooteps_f). YMAX is 210828714 as a float.
#define SINE_YMAX 0x1.921fb6p+27f
#define SINE_HALF_PI 0x1.921fb6p+0f
#define SINE_ONE_OVER_PI 0x1.45f306p-2f
#define SINE_PI 0x1.921fb54442d18p+1
#define SINE_ROOTEPS 0x1.6a09e6p-13f
#define SINE_R0 -0x1.555548p-3f
#define SINE_R1 0x1.110e7cp-7f
#define SINE_R2 -0x1.9f6444p-13f
#define SINE_R3 0x1.5d38b6p-19f

static inline float fsel(double a, float b, float c)
{
    float r;

    __asm__("fsel %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(b), "f"(c));
    return r;
}

// For |x| <= YMAX. N = (int) (y / pi + 0.5) is truncated in the FPRs (sinef: fctiwz and an integer), and it is odd
// where it differs from 2 trunc(t / 2).
static inline float mathfp_sinef(float x, int cosine)
{
    float ax = __builtin_fabsf(x);
    float y = cosine ? ax + SINE_HALF_PI : ax;
    double t = (double) (y * SINE_ONE_OVER_PI) + 0.5;
    double n = (double) (long long) t;
    double h = (double) (long long) (t * 0.5);
    float sgn = fsel(h + h - n, 1.0f, -1.0f);
    float xn = (float) n;
    float g, r;

    if (cosine) {
        xn -= 0.5f;
    } else {
        sgn = fsel(x, sgn, -sgn);
    }
    y = (float) __builtin_fma(-(double) xn, SINE_PI, ax);
    if (!(-SINE_ROOTEPS < y && y < SINE_ROOTEPS)) {
        g = y * y;
        r = __builtin_fmaf(__builtin_fmaf(__builtin_fmaf(g, SINE_R3, SINE_R2), g, SINE_R1), g, SINE_R0) * g;
        y = __builtin_fmaf(y, r, y);
    }
    return sgn * y;
}

// Outside |x| <= YMAX, NaN and Inf. Local, so the branch here is a sibling call and the common path has no frame.
static __attribute__((noinline)) float libm_sinf(float x)
{
    return sinf(x);
}

static __attribute__((noinline)) float libm_cosf(float x)
{
    return cosf(x);
}

float ps3_sinf(float x)
{
    if (!(__builtin_fabsf(x) <= SINE_YMAX)) {
        return libm_sinf(x);
    }
    return mathfp_sinef(x, 0);
}

float ps3_cosf(float x)
{
    if (!(__builtin_fabsf(x) <= SINE_YMAX)) {
        return libm_cosf(x);
    }
    return mathfp_sinef(x, 1);
}

// GCC 13 (the host build) merges sinf and cosf of one value into sincosf; ppu-gcc 7.2 does not.
void ps3_sincosf(float x, float* s, float* c)
{
    *s = ps3_sinf(x);
    *c = ps3_cosf(x);
}
