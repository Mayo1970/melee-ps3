// For a trace build only (EXTRA_SRCS, with -Wl,--wrap on these calls): logs how the match creates its fighters,
// to find the bad fighter kind or file name that stops the main thread in lbFileGetFullName.
#include <dolphin/os.h>
#include <melee/ft/forward.h>
#include <melee/pl/player.h>
#include <melee/pl/types.h>

#include "../os/diag.h"

struct StringPair {
    char* a;
    char* b;
};

extern struct StringPair ftData_803C1F40[];

// Run 19: Peach's file name pointer (kind 9) was NULL at Fighter_Create; the call trace hook finds the writer.
__attribute__((constructor)) static void watch_peach(void)
{
    ps3_diag.watch_value = *(unsigned long long*) &ftData_803C1F40[9].a;
    ps3_diag.watch = (unsigned long long*) &ftData_803C1F40[9].a;
}

void __real_Player_80031AD0(int slot);
Fighter_GObj* __real_Fighter_Create(struct plAllocInfo* input);
void __real_ftData_8008572C(FighterKind kind);

void __wrap_Player_80031AD0(int slot)
{
    OSReport("trace: player %d: slot type %d, character %d\n", slot, Player_GetPlayerSlotType(slot),
             Player_GetPlayerCharacter(slot));
    __real_Player_80031AD0(slot);
    OSReport("trace: player %d done\n", slot);
}

Fighter_GObj* __wrap_Fighter_Create(struct plAllocInfo* input)
{
    Fighter_GObj* gobj;

    OSReport("trace: Fighter_Create kind %d, slot %d, x5 %d, b0 %d\n", input->internal_id, input->slot, input->x5,
             input->x6.b0);
    gobj = __real_Fighter_Create(input);
    OSReport("trace: Fighter_Create -> %p\n", (void*) gobj);
    return gobj;
}

void __wrap_ftData_8008572C(FighterKind kind)
{
    if ((unsigned) kind < Ft_Kind_Max) {
        OSReport("trace: ftData_8008572C kind %d: %p %s\n", kind, (void*) ftData_803C1F40[kind].a,
                 ftData_803C1F40[kind].a != NULL ? ftData_803C1F40[kind].a : "(null)");
    } else {
        OSReport("trace: ftData_8008572C kind %d: out of range\n", kind);
    }
    __real_ftData_8008572C(kind);
}
