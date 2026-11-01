// The rollback's SPU thread through PSL1GHT, as the vertex program's (gx/vxjob_cell.c). No SDK header here.
#include <stdbool.h>
#include <stdint.h>

#include <lv2/spu.h>
#include <sys/spu.h>

#include "rb_shared.h"

void OSReport(char* fmt, ...);
bool rb_start(RbShared* shared);

// bin2s names the embedded SPU ELF after its file.
extern const u8 rb_spu_elf[];

bool rb_start(RbShared* shared)
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
    r = sysSpuImageImport(&image, rb_spu_elf, SPU_IMAGE_PROTECT);
    if (r != 0) {
        OSReport("rollback: sysSpuImageImport failed (%08x)\n", (u32) r);
        return false;
    }
    sysSpuThreadGroupAttributeInitialize(gattr);
    sysSpuThreadGroupAttributeName(gattr, "rb_spu");
    r = sysSpuThreadGroupCreate(&group, 1, 100, &gattr);
    sysSpuThreadAttributeInitialize(tattr);
    sysSpuThreadAttributeName(tattr, "rb_spu");
    sysSpuThreadArgumentInitialize(arg);
    arg.arg0 = (u64) (uintptr_t) shared;
    r = r != 0 ? r : sysSpuThreadInitialize(&thread, group, 0, &image, &tattr, &arg);
    r = r != 0 ? r : sysSpuThreadGroupStart(group);
    if (r != 0) {
        OSReport("rollback: SPU setup failed (%08x)\n", (u32) r);
        return false;
    }
    return true;
}
