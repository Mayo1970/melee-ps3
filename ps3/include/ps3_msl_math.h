// Forced into the game sources (Makefile): the C library calls whose GameCube MSL results differ from libm's.
#ifndef PS3_MSL_MATH_H
#define PS3_MSL_MATH_H

#include <math.h>

#include <ps3_trig.h>

// MSL's sqrtf (math_ppc.h) returns x for x <= 0; libm gives NaN for a negative x, as from rounding in 1 - x * x.
static inline float ps3_msl_sqrtf(float x)
{
    return x > 0.0f ? __builtin_sqrtf(x) : x;
}
#define sqrtf ps3_msl_sqrtf

// MWCC converts a float to a 32-bit unsigned with __cvt_fp2unsigned (runtime.c): 0 below 0 and for NaN, 0xFFFFFFFF
// from 2^32 up. GCC goes through a 64-bit integer, so -1.0 becomes 0xFFFFFFFF.
static inline unsigned int ps3_mwcc_f2u(double x)
{
    return !(x >= 0.0) ? 0u : x >= 4294967296.0 ? 0xFFFFFFFFu : (unsigned int) x;
}

#endif
