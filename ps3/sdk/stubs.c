// SDK parts the port leaves out: the USB debugger link (MCC, FIO).
#include <dolphin/types.h>
#include <dolphin/mcc.h>

// No USB debugger is attached.
int MCCInit(enum MCC_EXI exiChannel, u8 timeout, MCC_CBSysEvent callbackSysEvent)
{
    return 0;
}

void MCCExit(void) {}

int MCCEnumDevices(MCC_CBEnumDevices callbackEnumDevices)
{
    return 0;
}

u8 MCCGetFreeBlocks(enum MCC_MODE mode)
{
    return 0;
}

u8 MCCGetLastError(void)
{
    return 0;
}

int MCCGetConnectionStatus(enum MCC_CHANNEL chID, enum MCC_CONNECT* connect)
{
    return 0;
}

int MCCNotify(enum MCC_CHANNEL chID, u32 notify)
{
    return 0;
}

int MCCOpen(enum MCC_CHANNEL chID, u8 blockSize, MCC_CBEvent callbackEvent)
{
    return 0;
}

int MCCClose(enum MCC_CHANNEL chID)
{
    return 0;
}

int MCCStreamOpen(enum MCC_CHANNEL chID, u8 blockSize)
{
    return 0;
}

int MCCRead(enum MCC_CHANNEL chID, u32 offset, void* data, long size, enum MCC_SYNC_STATE async)
{
    return 0;
}

int MCCWrite(enum MCC_CHANNEL chID, u32 offset, void* data, long size, enum MCC_SYNC_STATE async)
{
    return 0;
}

int FIOInit(enum MCC_EXI exiChannel, enum MCC_CHANNEL chID, u8 blockSize)
{
    return 0;
}

void FIOExit(void) {}

int FIOQuery(void)
{
    return 0;
}

int FIOFopen(const char* filename, u32 mode)
{
    return -1;
}

int FIOFclose(int handle)
{
    return 0;
}

u32 FIOFwrite(int handle, void* data, u32 size)
{
    return 0;
}
