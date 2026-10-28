// The PPCArch calls the SDK makes: GX's for the write-gather pipe, which the PS3 lacks (see ps3/gx/fifo.c),
// and the crash handler's.
#include <dolphin/base/PPCArch.h>

#include <gx_pipe.h>

u32 PPCMfhid2(void)
{
    return 0;
}

void PPCMthid2(u32 newhid2) {}

// Bit 0 clear: the gather buffer is empty.
u32 PPCMfwpar(void)
{
    return 0;
}

void PPCMtwpar(u32 newwpar) {}

// GXFlush ends with PPCSync to push the gather buffer out; here that publishes the pipe to the GP.
void PPCSync(void)
{
    __asm__ volatile("sync" ::: "memory");
    __GXPipeFlush();
}

// The crash handler sets the FP exception bits; lv2 owns the MSR, so they stay off.
u32 PPCMfmsr(void)
{
    return 0;
}

void PPCMtmsr(u32 newMSR) {}
