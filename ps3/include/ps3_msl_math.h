// Forced into the game sources (Makefile): the C library calls whose GameCube MSL results differ from libm's.
#ifndef PS3_MSL_MATH_H
#define PS3_MSL_MATH_H

#include <math.h>

// MSL's sqrtf (math_ppc.h) returns x for x <= 0; libm gives NaN for a negative x, as from rounding in 1 - x * x.
static inline float ps3_msl_sqrtf(float x)
{
    return x > 0.0f ? __builtin_sqrtf(x) : x;
}
#define sqrtf ps3_msl_sqrtf

#endif
