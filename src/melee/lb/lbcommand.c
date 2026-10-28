#include "lbcommand.h"

#include "inlines.h"
#include "lb_0219.h"
#include "types.h"
#ifdef TARGET_PS3
#include <ps3_dat.h>
#endif

void (*lbCommand_803B9840[16])(CommandInfo*) = {
    Command_00, Command_01, Command_02, Command_03, Command_04, Command_05,
    Command_06, Command_07, Command_08, Command_09, NULL,       NULL,
    NULL,       NULL,       NULL,       NULL
};

/// Reset
void Command_00(CommandInfo* info)
{
    info->x8.u = NULL;
}

/// SynchronousTimer
void Command_01(CommandInfo* info)
{
    info->timer += info->x8.u->Command_00.value;
    NEXT_CMD(info);
}

/// AsynchronousTimer
void Command_02(CommandInfo* info)
{
    info->timer = info->x8.u->Command_02.value - info->frame_count;
    NEXT_CMD(info);
}

/// SetLoop
void Command_03(CommandInfo* info)
{
    info->event_return[info->loop_count++] = info->x8.u + 1;
    info->event_return[info->loop_count++] =
        (union CmdUnion*) info->x8.u->Command_03.value;
    NEXT_CMD(info);
}

/// Execute Loop
void Command_04(CommandInfo* info)
{
#ifdef TARGET_PS3
    // The GameCube reaches the loop's count and start as 32-bit words from
    // info; they are the last two event_return entries (Command_03).
    union CmdUnion** top = &info->event_return[info->loop_count - 1];

    *top = (union CmdUnion*) ((uintptr_t) *top - 1);
    if ((s32) (uintptr_t) *top) {
        info->x8.u = top[-1];
        return;
    }
#else
    u32* ptr = (u32*) info;
    ptr[info->loop_count + 3] -= 1;

    if ((s32) info->event_return[info->loop_count - 1]) {
        info->x8.ptr[0] = &info->x8.ptr[info->loop_count][0];
        return;
    }
#endif
    NEXT_CMD(info);
    info->loop_count -= 2;
}

/// Subroutine
void Command_05(CommandInfo* info)
{
    NEXT_CMD(info);
    info->event_return[info->loop_count++] = info->x8.u + 1;
#ifdef TARGET_PS3
    info->x8.u = ps3_dat_script(info->x8.u, info->x8.u->Command_05.ptr);
#else
    info->x8.u = info->x8.u->Command_05.ptr;
#endif
}

/// Return
void Command_06(CommandInfo* info)
{
    info->x8.u = info->event_return[info->loop_count -= 1];
}

/// Goto
void Command_07(CommandInfo* info)
{
    NEXT_CMD(info);
#ifdef TARGET_PS3
    info->x8.u = ps3_dat_script(info->x8.u, info->x8.u->Command_07.ptr);
#else
    info->x8.u = info->x8.u->Command_07.ptr;
#endif
}

/// SetTimerAnimation
void Command_08(CommandInfo* info)
{
    NEXT_CMD(info);
    info->timer = F32_MAX;
}

void Command_09(CommandInfo* info)
{
    lbBgFlash_80021C48(info->x8.u->Command_09.param_1,
                       info->x8.u->Command_09.param_2);
    NEXT_CMD(info);
}

bool Command_Execute(CommandInfo* info, u32 command)
{
    if (command < 10) {
        lbCommand_803B9840[command](info);
        return true;
    }
    return false;
}
