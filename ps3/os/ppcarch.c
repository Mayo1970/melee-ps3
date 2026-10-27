// The PPCArch calls GX makes for the write-gather pipe. The PS3 has no pipe (see ps3/gx/fifo.c).
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
