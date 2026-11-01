// The game's fobj.c with its static parseFloat reachable, for math_test.c (Makefile mathobjs).
#include "../../src/sysdolphin/baselib/fobj.c"

f32 (*math_parseFloat)(u8** pos, u8 frac) = parseFloat;
