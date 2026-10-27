// The audio backend on the PS3: a 2-channel lv2 audio port, the audio program on one SPU, and a thread
// that turns the program's events into calls to ps3_audio_spu_event.
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <audio/audio.h>
#include <lv2/spu.h>
#include <sys/event_queue.h>
#include <sys/spu.h>

#include "audio_backend.h"

// PSL1GHT's u64 is unsigned long and the SDK's is unsigned long long, so no SDK header comes in here.
void OSReport(char* fmt, ...);

// bin2s names the embedded SPU ELF after its file.
extern const u8 ax_spu_elf[];

static sysSpuImage image;
static sys_spu_group_t group;
static sys_spu_thread_t thread;
static sys_event_queue_t queue;
static u32 port;
static pthread_t event_thread;

static void* event_main(void* arg)
{
    for (;;) {
        sys_event_t ev;
        if (sysEventQueueReceive(queue, &ev, 0) != 0) {
            return NULL;
        }
        // An SPU user event carries the program's data0 in the low 24 bits of data_2.
        ps3_audio_spu_event((uint32_t) ev.data_2 & 0xFFFFFF);
    }
}

static bool open_port(AxShared* shared)
{
    audioPortParam params;
    audioPortConfig config;

    memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_BLOCK_16;
    if (audioInit() != 0 || audioPortOpen(&params, &port) != 0 ||
        audioGetPortConfig(port, &config) != 0)
    {
        return false;
    }
    shared->port_data_ea = config.audioDataStart;
    shared->port_read_ea = config.readIndex;
    shared->port_blocks = (uint32_t) config.numBlocks;
    return audioPortStart(port) == 0;
}

bool ps3_audio_backend_start(AxShared* shared)
{
    sys_event_queue_attr_t qattr = { SYS_EVENT_QUEUE_FIFO, SYS_EVENT_QUEUE_PPU, "axspu" };
    sysSpuThreadGroupAttribute gattr;
    sysSpuThreadAttribute tattr;
    sysSpuThreadArgument arg;
    s32 r;

    if (!open_port(shared)) {
        OSReport("audio: the audio port did not open\n");
        return false;
    }
    // Several SPU users share one process; an error here only means it is set up already.
    sysSpuInitialize(6, 0);
    r = sysSpuImageImport(&image, ax_spu_elf, SPU_IMAGE_PROTECT);
    if (r != 0) {
        OSReport("audio: sysSpuImageImport failed (%08x)\n", (u32) r);
        return false;
    }
    sysSpuThreadGroupAttributeInitialize(gattr);
    sysSpuThreadGroupAttributeName(gattr, "ax_spu");
    r = sysSpuThreadGroupCreate(&group, 1, 100, &gattr);
    sysSpuThreadAttributeInitialize(tattr);
    sysSpuThreadAttributeName(tattr, "ax_spu");
    sysSpuThreadArgumentInitialize(arg);
    arg.arg0 = (u64) (uintptr_t) shared;
    r = r != 0 ? r : sysSpuThreadInitialize(&thread, group, 0, &image, &tattr, &arg);
    r = r != 0 ? r : sysEventQueueCreate(&queue, &qattr, SYS_EVENT_QUEUE_KEY_LOCAL, 32);
    // Connected before the start: the program's first event is AX_EV_INIT.
    r = r != 0 ? r : sysSpuThreadConnectEvent(thread, queue, SPU_THREAD_EVENT_USER, AX_EVENT_PORT);
    if (r != 0) {
        OSReport("audio: SPU setup failed (%08x)\n", (u32) r);
        return false;
    }
    pthread_create(&event_thread, NULL, event_main, NULL);
    r = sysSpuThreadGroupStart(group);
    if (r != 0) {
        OSReport("audio: sysSpuThreadGroupStart failed (%08x)\n", (u32) r);
        return false;
    }
    return true;
}

void ps3_audio_backend_mail(uint32_t mail)
{
    int tries;

    // The inbound mailbox holds four mails; the program empties it within an AI period.
    for (tries = 0; sysSpuThreadWriteMb(thread, mail) != 0; tries++) {
        if (tries == 1000) {
            OSReport("audio: the audio program takes no mail\n");
            return;
        }
        usleep(50);
    }
}
