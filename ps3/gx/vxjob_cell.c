// The vertex program's SPU thread through PSL1GHT, as the audio program's (audio_cell.c). No SDK header here.
#include <stdbool.h>
#include <stdint.h>

#include <lv2/spu.h>
#include <sys/spu.h>

#include "vx_shared.h"

void OSReport(char* fmt, ...);
bool vx_start(VxShared* shared);

// bin2s names the embedded SPU ELF after its file.
extern const u8 vx_spu_elf[];

bool vx_start(VxShared* shared)
{
    static sysSpuImage image;
    static sys_spu_group_t group;
    static sys_spu_thread_t thread;
    sysSpuThreadGroupAttribute gattr;
    sysSpuThreadAttribute tattr;
    sysSpuThreadArgument arg;
    s32 r;

    // Several SPU users share one process; an error here only means it is set up already.
    sysSpuInitialize(6, 0);
    r = sysSpuImageImport(&image, vx_spu_elf, SPU_IMAGE_PROTECT);
    if (r != 0) {
        OSReport("vx: sysSpuImageImport failed (%08x)\n", (u32) r);
        return false;
    }
    sysSpuThreadGroupAttributeInitialize(gattr);
    sysSpuThreadGroupAttributeName(gattr, "vx_spu");
    r = sysSpuThreadGroupCreate(&group, 1, 100, &gattr);
    sysSpuThreadAttributeInitialize(tattr);
    sysSpuThreadAttributeName(tattr, "vx_spu");
    sysSpuThreadArgumentInitialize(arg);
    arg.arg0 = (u64) (uintptr_t) shared;
    r = r != 0 ? r : sysSpuThreadInitialize(&thread, group, 0, &image, &tattr, &arg);
    r = r != 0 ? r : sysSpuThreadGroupStart(group);
    if (r != 0) {
        OSReport("vx: SPU setup failed (%08x)\n", (u32) r);
        return false;
    }
    return true;
}
