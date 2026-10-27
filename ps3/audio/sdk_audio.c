// The GameCube DSP, AI and ARAM under the SDK's AX library and ARQ. The SPU audio program does the DSP's
// and the AI's work; here their SDK calls keep state, and their callbacks run as interrupts.
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/ax.h>
#include <dolphin/dsp.h>
#include <dolphin/os.h>

#include "../os/intr.h"
#include "audio_backend.h"
#include "ax_shared.h"

#define ARAM_USER_BASE 0x4000
#define MAX_ARQ_PENDING 64

static AxShared shared __attribute__((aligned(128)));
static u8* aram;
static bool started;
static bool spu_ready;

// Set on the backend's thread, taken by the interrupt handlers.
static unsigned init_pending;
static unsigned dsp_pending;
static unsigned ai_pending;

static DSPTaskInfo* dsp_task;
static bool dsp_initialized;
static AIDCallback ai_callback;
static u32 dsp_rate = AI_SAMPLERATE_32KHZ;
static u8 stream_vol_left, stream_vol_right;

// AXOut points its DSP task at the AX microcode. The SPU program replaces it, so these stay empty.
u16 axDspSlaveLength;
u16 axDspSlave[AX_DSP_SLAVE_LENGTH];

static u32* ar_block_length;
static u32 ar_free_blocks;
static u32 ar_stack_pointer;
static bool ar_initialized;
static ARQCallback ar_dma_callback;
static u32 arq_chunk_size = 0x1000;

static struct {
    ARQRequest* request;
} arq_pending[MAX_ARQ_PENDING];
static u32 arq_head, arq_tail;

static void ensure_aram(void)
{
    if (aram == NULL) {
        // 128-byte aligned for the SPU's cache lines.
        aram = memalign(128, AX_ARAM_SIZE);
        memset(aram, 0, AX_ARAM_SIZE);
        shared.aram_ea = (u64) (uintptr_t) aram;
    }
}

static void dsp_interrupt(void)
{
    unsigned n;

    if (__atomic_exchange_n(&init_pending, 0, __ATOMIC_ACQ_REL) && dsp_task != NULL &&
        dsp_task->init_cb != NULL)
    {
        dsp_task->init_cb(dsp_task);
    }
    // Each finished command list is one DSP yield; AX resumes its frame logic from it.
    for (n = __atomic_exchange_n(&dsp_pending, 0, __ATOMIC_ACQ_REL); n != 0; n--) {
        if (dsp_task != NULL && dsp_task->res_cb != NULL) {
            dsp_task->res_cb(dsp_task);
        }
    }
}

static void ai_interrupt(void)
{
    unsigned n;

    for (n = __atomic_exchange_n(&ai_pending, 0, __ATOMIC_ACQ_REL); n != 0; n--) {
        if (ai_callback != NULL) {
            ai_callback();
        }
    }
}

static void aram_interrupt(void)
{
    for (;;) {
        BOOL enabled = OSDisableInterrupts();
        ARQRequest* request;

        if (arq_head == arq_tail) {
            OSRestoreInterrupts(enabled);
            return;
        }
        request = arq_pending[arq_head++ % MAX_ARQ_PENDING].request;
        OSRestoreInterrupts(enabled);
        if (request->callback != NULL) {
            request->callback(request);
        }
    }
}

void ps3_audio_spu_event(uint32_t code)
{
    switch (code) {
    case AX_EV_INIT:
        __atomic_store_n(&spu_ready, true, __ATOMIC_RELEASE);
        if (dsp_task != NULL) {
            __atomic_store_n(&init_pending, 1, __ATOMIC_RELEASE);
        }
        ps3_intr_raise(PS3_INTR_DSP);
        break;
    case AX_EV_DSP:
        __atomic_add_fetch(&dsp_pending, 1, __ATOMIC_ACQ_REL);
        ps3_intr_raise(PS3_INTR_DSP);
        break;
    case AX_EV_AI:
        __atomic_add_fetch(&ai_pending, 1, __ATOMIC_ACQ_REL);
        ps3_intr_raise(PS3_INTR_AI);
        break;
    }
}

static void start(void)
{
    if (started) {
        return;
    }
    started = true;
    ensure_aram();
    ps3_intr_set_handler(PS3_INTR_ARAM, aram_interrupt);
    ps3_intr_set_handler(PS3_INTR_DSP, dsp_interrupt);
    ps3_intr_set_handler(PS3_INTR_AI, ai_interrupt);
    if (!ps3_audio_backend_start(&shared)) {
        OSReport("audio: no audio program, so no sound\n");
    }
}

// The AX statistics the audio program keeps, for the log.
void ps3_audio_stats(u32* frames, u32* ticks, u32* silent_ticks, u32* bad_commands)
{
    *frames = shared.frames;
    *ticks = shared.ticks;
    *silent_ticks = shared.silent_ticks;
    *bad_commands = shared.bad_commands;
}

void DSPInit(void)
{
    start();
    dsp_initialized = true;
}

BOOL DSPCheckInit(void)
{
    return dsp_initialized;
}

DSPTaskInfo* DSPAddTask(DSPTaskInfo* t)
{
    dsp_task = t;
    if (__atomic_load_n(&spu_ready, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&init_pending, 1, __ATOMIC_RELEASE);
        ps3_intr_raise(PS3_INTR_DSP);
    }
    return t;
}

DSPTaskInfo* DSPCancelTask(DSPTaskInfo* t)
{
    if (dsp_task == t) {
        dsp_task = NULL;
    }
    return t;
}

// AX asks for its task when the AI period comes before the DSP finished; one task never waits.
DSPTaskInfo* DSPAssertTask(DSPTaskInfo* t)
{
    return t;
}

DSPTaskInfo* __DSPGetCurrentTask(void)
{
    return dsp_task;
}

void DSPSendMailToDSP(u32 mail)
{
    ps3_audio_backend_mail(mail);
}

// The backend's mail call returns once the mailbox took the mail.
u32 DSPCheckMailToDSP(void)
{
    return 0;
}

u32 DSPCheckMailFromDSP(void)
{
    return 0;
}

u32 DSPReadMailFromDSP(void)
{
    return 0;
}

u32 DSPReadCPUToDSPMbox(void)
{
    return 0;
}

void DSPAssertInt(void) {}

// AXQuit stops the AI first; the program then only writes silence.
void DSPHalt(void) {}

void DSPUnhalt(void) {}

void DSPReset(void) {}

u32 DSPGetDMAStatus(void)
{
    return 0;
}

void AIInit(u8* stack)
{
    start();
}

void AIReset(void) {}

BOOL AICheckInit(void)
{
    return started;
}

AIDCallback AIRegisterDMACallback(AIDCallback callback)
{
    AIDCallback old = ai_callback;
    ai_callback = callback;
    return old;
}

void AIInitDMA(u32 start_addr, u32 length)
{
    shared.ai_addr = start_addr;
    shared.ai_length = length;
    __sync_synchronize();
}

void AIStartDMA(void)
{
    shared.ai_running = 1;
    __sync_synchronize();
}

void AIStopDMA(void)
{
    shared.ai_running = 0;
    __sync_synchronize();
}

BOOL AIGetDMAEnableFlag(void)
{
    return shared.ai_running != 0;
}

u32 AIGetDMAStartAddr(void)
{
    return shared.ai_addr;
}

u32 AIGetDMALength(void)
{
    return shared.ai_length;
}

u32 AIGetDMABytesLeft(void)
{
    return 0;
}

void AISetDSPSampleRate(u32 rate)
{
    // The audio program converts from 32 kHz, the rate AX always mixes at.
    if (rate != AI_SAMPLERATE_32KHZ) {
        OSReport("audio: AI DSP rate %u is not supported; 32 kHz stays\n", rate);
    }
    dsp_rate = rate;
}

u32 AIGetDSPSampleRate(void)
{
    return dsp_rate;
}

// Melee mixes its music with AX, so the disc stream (DTK) stays silent.
void AISetStreamVolLeft(u8 vol)
{
    stream_vol_left = vol;
}

u8 AIGetStreamVolLeft(void)
{
    return stream_vol_left;
}

void AISetStreamVolRight(u8 vol)
{
    stream_vol_right = vol;
}

u8 AIGetStreamVolRight(void)
{
    return stream_vol_right;
}

u32 ARInit(u32* stack_index_addr, u32 num_entries)
{
    BOOL enabled;

    ensure_aram();
    if (ar_initialized) {
        return ARAM_USER_BASE;
    }
    enabled = OSDisableInterrupts();
    ar_stack_pointer = ARAM_USER_BASE;
    ar_free_blocks = num_entries;
    ar_block_length = stack_index_addr;
    ar_initialized = true;
    OSRestoreInterrupts(enabled);
    return ar_stack_pointer;
}

int ARCheckInit(void)
{
    return ar_initialized;
}

void ARReset(void) {}

void ARSetSize(void) {}

u32 ARGetBaseAddress(void)
{
    return ARAM_USER_BASE;
}

u32 ARGetSize(void)
{
    return AX_ARAM_SIZE;
}

u32 ARAlloc(u32 length)
{
    BOOL enabled = OSDisableInterrupts();
    u32 addr = ar_stack_pointer;

    if (ar_free_blocks == 0 || length > AX_ARAM_SIZE - ar_stack_pointer) {
        OSReport("audio: ARAlloc(0x%x) at 0x%x runs past ARAM\n", length, ar_stack_pointer);
    }
    ar_stack_pointer += length;
    *ar_block_length++ = length;
    ar_free_blocks--;
    OSRestoreInterrupts(enabled);
    return addr;
}

u32 ARFree(u32* length)
{
    BOOL enabled = OSDisableInterrupts();
    u32 sp;

    ar_block_length--;
    if (length != NULL) {
        *length = *ar_block_length;
    }
    ar_stack_pointer -= *ar_block_length;
    ar_free_blocks++;
    sp = ar_stack_pointer;
    OSRestoreInterrupts(enabled);
    return sp;
}

// ARAM addresses wrap at 16 MB, as the GameCube's DMA does.
static void aram_copy(u32 type, u32 mram, u32 aram_addr, u32 length)
{
    u8* m = (u8*) (uintptr_t) mram;

    ensure_aram();
    while (length != 0) {
        u32 at = aram_addr & (AX_ARAM_SIZE - 1);
        u32 n = length < AX_ARAM_SIZE - at ? length : AX_ARAM_SIZE - at;
        if (type == ARAM_DIR_MRAM_TO_ARAM) {
            memcpy(aram + at, m, n);
        } else {
            memcpy(m, aram + at, n);
        }
        m += n;
        aram_addr += n;
        length -= n;
    }
}

ARQCallback ARRegisterDMACallback(ARQCallback callback)
{
    ARQCallback old = ar_dma_callback;
    ar_dma_callback = callback;
    return old;
}

u32 ARGetDMAStatus(void)
{
    return 0;
}

void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length)
{
    aram_copy(type, mainmem_addr, aram_addr, length);
}

void ARQInit(void)
{
    ensure_aram();
    ps3_intr_set_handler(PS3_INTR_ARAM, aram_interrupt);
}

void ARQReset(void) {}

// The copy happens in the call; the callback waits for the ARAM interrupt, as devcom expects.
void ARQPostRequest(ARQRequest* request, uintptr_t owner, u32 type, u32 priority, u32 source,
                    u32 dest, u32 length, ARQCallback callback)
{
    BOOL enabled;

    request->next = NULL;
    request->owner = owner;
    request->type = type;
    request->priority = priority;
    request->source = source;
    request->dest = dest;
    request->length = length;
    request->callback = callback;
    if (type == ARQ_TYPE_MRAM_TO_ARAM) {
        aram_copy(type, source, dest, length);
    } else {
        aram_copy(type, dest, source, length);
    }
    if (callback == NULL) {
        return;
    }
    enabled = OSDisableInterrupts();
    if (arq_tail - arq_head == MAX_ARQ_PENDING) {
        OSRestoreInterrupts(enabled);
        OSReport("audio: %d ARQ callbacks wait; one more is dropped\n", MAX_ARQ_PENDING);
        return;
    }
    arq_pending[arq_tail++ % MAX_ARQ_PENDING].request = request;
    OSRestoreInterrupts(enabled);
    ps3_intr_raise(PS3_INTR_ARAM);
}

// Requests finish as they are posted, so only callbacks still waiting can be removed.
void ARQRemoveOwnerRequest(uintptr_t owner)
{
    BOOL enabled = OSDisableInterrupts();
    u32 i;

    for (i = arq_head; i != arq_tail; i++) {
        ARQRequest* r = arq_pending[i % MAX_ARQ_PENDING].request;
        if (r != NULL && r->owner == owner) {
            r->callback = NULL;
        }
    }
    OSRestoreInterrupts(enabled);
}

void ARQRemoveRequest(ARQRequest* request)
{
    request->callback = NULL;
}

void ARQFlushQueue(void) {}

void ARQSetChunkSize(u32 size)
{
    arq_chunk_size = size;
}

u32 ARQGetChunkSize(void)
{
    return arq_chunk_size;
}
