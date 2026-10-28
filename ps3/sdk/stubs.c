// SDK parts the port leaves out: the AXFX effects (no reverb, chorus or delay yet), THP video (no intro movie)
// and the USB debugger link (MCC, FIO).
#include <dolphin/axfx.h>
#include <dolphin/mcc.h>
#include <dolphin/thp/thp.h>

// An effect that fails to start leaves the AUX bus without a callback (axdriver.c).
int AXFXReverbHiInit(struct AXFX_REVERBHI* rev)
{
    return 0;
}

int AXFXReverbHiShutdown(struct AXFX_REVERBHI* rev)
{
    return 1;
}

void AXFXReverbHiCallback(struct AXFX_BUFFERUPDATE* bufferUpdate, struct AXFX_REVERBHI* reverb) {}

int AXFXReverbStdInit(struct AXFX_REVERBSTD* rev)
{
    return 0;
}

int AXFXReverbStdShutdown(struct AXFX_REVERBSTD* rev)
{
    return 1;
}

void AXFXReverbStdCallback(struct AXFX_BUFFERUPDATE* bufferUpdate, struct AXFX_REVERBSTD* reverb) {}

int AXFXChorusInit(struct AXFX_CHORUS* c)
{
    return 0;
}

int AXFXChorusShutdown(struct AXFX_CHORUS* c)
{
    return 1;
}

void AXFXChorusCallback(struct AXFX_BUFFERUPDATE* bufferUpdate, struct AXFX_CHORUS* chorus) {}

int AXFXDelayInit(struct AXFX_DELAY* delay)
{
    return 0;
}

int AXFXDelayShutdown(struct AXFX_DELAY* delay)
{
    return 1;
}

void AXFXDelayCallback(struct AXFX_BUFFERUPDATE* bufferUpdate, struct AXFX_DELAY* delay) {}

void AXFXSetHooks(void* (*alloc_hook)(unsigned long), void (*free_hook)(void*)) {}

// THP: the work buffer passes through and no frame is decoded.
void THPInit(void) {}

s32 THPDec_8032F8D4(u8* data, THPDec_8032FD40_Data* out)
{
    return 0;
}

s32 THPDec_8032FD40(THPDec_8032FD40_Data* data, u16 num)
{
    return 32;
}

THPFileInfo* THPVideoDecode(void* header, void* status_out, THPFileInfo* work, void* data, THPDec_8032FD40_Data* desc)
{
    return work;
}

void THPDec_80331340(THPFileInfo* info, void* tileY, void* tileU, void* tileV) {}

void THPDec_803313D0(THPFileInfo* info, void* tileY, void* tileU, void* tileV, u32 width) {}

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
