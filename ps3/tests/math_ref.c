// The code that sdk/mtx.c, baselib mtx.c and fobj.c replaced on the PS3, as it was, for math_test.c. Built by
// ppu-gcc like the game (Makefile mathobjs), so it compiles to the instructions the game had.
#include <math.h>
#include <string.h>

#include <dolphin/mtx.h>
#include <sysdolphin/baselib/fobj.h>

#define EPSILON 0.0000000001f

void ref_PSMTXConcat(Mtx a, Mtx b, Mtx ab)
{
    Mtx t;
    int i;

    for (i = 0; i < 3; i++) {
        t[i][0] = a[i][0] * b[0][0] + a[i][1] * b[1][0] + a[i][2] * b[2][0];
        t[i][1] = a[i][0] * b[0][1] + a[i][1] * b[1][1] + a[i][2] * b[2][1];
        t[i][2] = a[i][0] * b[0][2] + a[i][1] * b[1][2] + a[i][2] * b[2][2];
        t[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3];
    }
    memcpy(ab, t, sizeof(Mtx));
}

static inline f32 fabsf_bitwise(f32 v)
{
    *(u32*) &v &= ~0x80000000;
    return v;
}

static inline f32 HSD_CalcDeterminantMatrix3x4(Mtx m)
{
    return m[0][0] * m[1][1] * m[2][2] + m[0][1] * m[1][2] * m[2][0] +
           m[0][2] * m[1][0] * m[2][1] - m[2][0] * m[1][1] * m[0][2] -
           m[1][0] * m[0][1] * m[2][2] - m[0][0] * m[2][1] * m[1][2];
}

void ref_HSD_MtxInverseTranspose(Mtx src, Mtx dest)
{
    Mtx* m;
    Mtx tempMatrix;
    f32 det = HSD_CalcDeterminantMatrix3x4(src);

    m = (Mtx*) src;

    if (fabsf_bitwise(det) < EPSILON) {
        if (*m != dest) {
            MTXCopy(*m, dest);
        }
    } else {
        if (*m == dest) {
            MTXCopy(*m, tempMatrix);
            m = &tempMatrix;
        }

        det = 1.0f / det;

        dest[0][0] =
            (((*m)[1][1] * (*m)[2][2]) - ((*m)[2][1] * (*m)[1][2])) * det;
        dest[1][0] =
            -(((*m)[0][1] * (*m)[2][2]) - ((*m)[2][1] * (*m)[0][2])) * det;
        dest[2][0] =
            (((*m)[0][1] * (*m)[1][2]) - ((*m)[1][1] * (*m)[0][2])) * det;
        dest[0][1] =
            -(((*m)[1][0] * (*m)[2][2]) - ((*m)[2][0] * (*m)[1][2])) * det;
        dest[1][1] =
            (((*m)[0][0] * (*m)[2][2]) - ((*m)[2][0] * (*m)[0][2])) * det;
        dest[2][1] =
            -(((*m)[0][0] * (*m)[1][2]) - ((*m)[1][0] * (*m)[0][2])) * det;
        dest[0][2] =
            (((*m)[1][0] * (*m)[2][1]) - ((*m)[2][0] * (*m)[1][1])) * det;
        dest[1][2] =
            -(((*m)[0][0] * (*m)[2][1]) - ((*m)[2][0] * (*m)[0][1])) * det;
        dest[2][2] =
            (((*m)[0][0] * (*m)[1][1]) - ((*m)[1][0] * (*m)[0][1])) * det;
        dest[0][3] = 0;
        dest[1][3] = 0;
        dest[2][3] = 0;
    }
}

void ref_HSD_MtxScaledAdd(Mtx arg0, Mtx arg1, Mtx arg2, f32 arg3)
{
    f32* arr0 = (&arg0[0][0]);
    f32* arr1 = (&arg1[0][0]);
    f32* arr2 = (&arg2[0][0]);

    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);

    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);

    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
}

union parseFloat_u {
    f32 f;
    u32 d;
};

f32 ref_parseFloat(u8** pos, u8 frac)
{
    union parseFloat_u u;
    f32 numer;
    s32 denom;

    if (frac == HSD_A_FRAC_FLOAT) {
        u.d = (s32) ((*pos)++)[0];
        u.d |= ((*pos)++)[0] << 8;
        u.d |= ((*pos)++)[0] << 16;
        u.d |= ((*pos)++)[0] << 24;
        return u.f;
    }

    denom = (1 << (frac & 0x1F));
    switch (frac & 0xE0) {
    case HSD_A_FRAC_S8:
        numer = (s8) (*pos)[0];
        *pos += 1;
        break;
    case HSD_A_FRAC_U8:
        numer = (*pos)[0];
        *pos += 1;
        break;
    case HSD_A_FRAC_S16:
        numer = ((s8) (*pos)[1] << 8) | (*pos)[0];
        *pos += 2;
        break;
    case HSD_A_FRAC_U16:
        numer = ((*pos)[1] << 8) | (*pos)[0];
        *pos += 2;
        break;
    default:
        return 0.0f;
    }
    return numer / denom;
}

// FObjUpdateAnim's spline step, before (u16 fterm) and after (its float copy).
f32 ref_fterm_recip(u16 fterm)
{
    return 1.0 / fterm;
}

f32 new_fterm_recip(f32 fterm_f)
{
    return 1.0f / fterm_f;
}
