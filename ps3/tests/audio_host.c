// Host stand-ins for the audio test: the audio program on a thread instead of an SPU, DMA as memcpy with
// the SPU's alignment rules checked, and an audio port that plays while the program stays ahead of it.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../audio/audio_backend.h"
#include "../audio/spu_io.h"
#include "audio_host.h"

#define PORT_BLOCKS 16
// The port plays a block once the program wrote this far past it, so a run never underflows.
#define PLAY_LAG (4 * AX_PORT_BLOCK)

static float ring[PORT_BLOCKS * AX_PORT_BLOCK * 2] __attribute__((aligned(128)));
static uint64_t read_index __attribute__((aligned(16)));
static AxShared* shared;
static pthread_t spu_thread, port_thread;

float host_capture[HOST_CAPTURE_FRAMES * 2];
volatile uint32_t host_captured;

static pthread_mutex_t mb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mb_cond = PTHREAD_COND_INITIALIZER;
static uint32_t mb[16];
static unsigned mb_head, mb_tail;

static void check(uint64_t ea, const void* ls, uint32_t size, const char* what)
{
    if ((ea & 15) != 0 || ((uintptr_t) ls & 15) != 0 || (size & 15) != 0 || size == 0) {
        fprintf(stderr, "%s breaks the SPU's DMA rules: ea %llx ls %p size %u\n", what,
                (unsigned long long) ea, ls, size);
        abort();
    }
}

void io_get(void* ls, uint64_t ea, uint32_t size)
{
    check(ea, ls, size, "get");
    memcpy(ls, (void*) (uintptr_t) ea, size);
}

void io_put(const void* ls, uint64_t ea, uint32_t size)
{
    check(ea, ls, size, "put");
    memcpy((void*) (uintptr_t) ea, ls, size);
}

int io_mail_count(void)
{
    int n;
    pthread_mutex_lock(&mb_lock);
    n = (int) (mb_tail - mb_head);
    pthread_mutex_unlock(&mb_lock);
    return n;
}

uint32_t io_mail_read(void)
{
    uint32_t v;
    pthread_mutex_lock(&mb_lock);
    while (mb_head == mb_tail) {
        pthread_cond_wait(&mb_cond, &mb_lock);
    }
    v = mb[mb_head++ % 16];
    pthread_mutex_unlock(&mb_lock);
    return v;
}

// The PPU's event thread would call this; here the program's thread calls it directly.
void io_event(uint32_t code)
{
    ps3_audio_spu_event(code);
}

void io_wait_us(uint32_t us)
{
    usleep(us);
}

void ps3_audio_backend_mail(uint32_t mail)
{
    pthread_mutex_lock(&mb_lock);
    mb[mb_tail++ % 16] = mail;
    pthread_cond_signal(&mb_cond);
    pthread_mutex_unlock(&mb_lock);
}

static void* spu_main(void* arg)
{
    ax_spu_main((uint64_t) (uintptr_t) arg);
    return NULL;
}

// The program starts writing at block 1, as the port plays block 0. Block k of its output starts
// (k - 1) blocks into what it wrote; the port moves to it once PLAY_LAG more is written, and keeps a copy.
static void* port_main(void* arg)
{
    uint64_t k = 1;
#ifdef HOST_AUDIO_REALTIME
    struct timespec start;

    clock_gettime(CLOCK_MONOTONIC, &start);
#endif

    for (;;) {
        uint64_t written = (uint64_t) __atomic_load_n(&shared->ticks, __ATOMIC_ACQUIRE) * AX_OUT_FRAMES;
        bool due = true;
#ifdef HOST_AUDIO_REALTIME
        // The host game build plays at 48 kHz, as the console's port, so the game's AX frames keep their pace.
        struct timespec t;
        int64_t ns;

        clock_gettime(CLOCK_MONOTONIC, &t);
        ns = (int64_t) (t.tv_sec - start.tv_sec) * 1000000000 + (t.tv_nsec - start.tv_nsec);
        due = (uint64_t) ns * 48 / 1000000 >= (k - 1) * AX_PORT_BLOCK;
#endif
        if (due && written >= k * AX_PORT_BLOCK + PLAY_LAG) {
            uint32_t at = (uint32_t) (k % PORT_BLOCKS) * AX_PORT_BLOCK * 2;
            __atomic_store_n(&read_index, k % PORT_BLOCKS, __ATOMIC_RELEASE);
            if (host_captured + AX_PORT_BLOCK <= HOST_CAPTURE_FRAMES) {
                memcpy(host_capture + host_captured * 2, ring + at, AX_PORT_BLOCK * 2 * sizeof(float));
                __atomic_store_n(&host_captured, host_captured + AX_PORT_BLOCK, __ATOMIC_RELEASE);
            }
            k++;
        } else {
            usleep(200);
        }
    }
    return NULL;
}

bool ps3_audio_backend_start(AxShared* s)
{
    shared = s;
    s->port_data_ea = (uint64_t) (uintptr_t) ring;
    s->port_read_ea = (uint64_t) (uintptr_t) &read_index;
    s->port_blocks = PORT_BLOCKS;
    read_index = 0;
    pthread_create(&spu_thread, NULL, spu_main, s);
    pthread_create(&port_thread, NULL, port_main, NULL);
    return true;
}
