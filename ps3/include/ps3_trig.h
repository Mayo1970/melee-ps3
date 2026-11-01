// sinf, cosf and sincosf call sdk/trig.c: libm's results without its nested calls and memory round trips. They stay
// GCC builtins (constant folding, sin(-x) as -sin(x)), so the code around each call stays as it was with libm's.
#ifndef PS3_TRIG_H
#define PS3_TRIG_H

#include <math.h>

float sinf(float) __asm__("ps3_sinf");
float cosf(float) __asm__("ps3_cosf");
void sincosf(float, float*, float*) __asm__("ps3_sincosf");

#endif
