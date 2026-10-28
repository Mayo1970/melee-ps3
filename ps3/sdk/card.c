// The memory card: no card in either slot. Saves on the HDD come later.
#include <dolphin/card.h>

#include "../os/diag.h"

void CARDInit(void) {}

BOOL CARDProbe(long chan)
{
    ps3_diag.card_calls++;
    return FALSE;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCallback, CARDCallback attachCallback)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDUnmount(s32 chan)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo, CARDCallback callback)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDDeleteAsync(s32 chan, char* fileName, CARDCallback callback)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName, CARDCallback callback)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fileInfo)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* fileInfo)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDClose(CARDFileInfo* fileInfo)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDReadAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, CARDCallback callback)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDWriteAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, CARDCallback callback)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* stat, CARDCallback callback)
{
    ps3_diag.card_calls++;
    return CARD_RESULT_NOCARD;
}

long CARDGetXferredBytes(long chan)
{
    ps3_diag.card_calls++;
    return 0;
}
