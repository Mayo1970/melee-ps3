// The main thread's frame phases and a sampler of its GObj callbacks, for the heartbeat (mainprof.c).
#ifndef PS3_OS_MAINPROF_H
#define PS3_OS_MAINPROF_H

#include <dolphin/types.h>

enum { MAIN_PHASE_OTHER, MAIN_PHASE_LOGIC, MAIN_PHASE_RENDER, MAIN_PHASE_XFB, MAIN_PHASE_COUNT };

void ps3_mainprof_start(void);
// One line: the samples' phases and the busiest callbacks (.opd addresses) since the last call.
void ps3_mainprof_log(u32 n);

#endif
