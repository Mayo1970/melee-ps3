// Interpreters of RSX vertex and fragment ucode, as the PC tests and the software RSX run it. They decode the
// fields vpgen and fpgen write (the cgcomp encoding), with float math.
#ifndef PS3_TESTS_RSX_INTERP_H
#define PS3_TESTS_RSX_INTERP_H

#include <dolphin/types.h>

// out is indexed by output register: 0 position, 1-2 colours, 5 fog, 6 point size, 7 + n texcoord n.
void vp_run(const u32 (*w)[4], int n, const float (*c)[4], const float in[16][4], float out[16][4]);

typedef void (*FpSampler)(void* user, int unit, const float coord[4], float out[4]);
// ucode: as fp_ucode writes it (halfwords swapped). in is indexed by input id (fpgen.h FIN_*). depth, if not NULL,
// gets R1.z, the depth export.
void fp_run(const u32* ucode, int words, const float in[16][4], FpSampler sample, void* user, float out[4],
            float* depth);

#endif
