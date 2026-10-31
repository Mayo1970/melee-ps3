// Host stand-ins for the vertex program: a thread instead of an SPU, DMA as memcpy. Addresses below 4 GB, as
// under qemu-ppc64 -R 0x100000000.
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "../gx/vx_shared.h"
#include "../gx/vxspu_io.h"

bool vx_start(VxShared* shared);

void vxio_get(void* ls, uint32_t ea, uint32_t size)
{
    memcpy(ls, (const void*) (uintptr_t) ea, size);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

void vxio_put(const void* ls, uint32_t ea, uint32_t size)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy((void*) (uintptr_t) ea, ls, size);
}

void vxio_gather(void* ls, const VxGather* list, uint32_t n)
{
    uint8_t* d = ls;
    uint32_t i;

    for (i = 0; i < n; i++) {
        memcpy(d, (const void*) (uintptr_t) list[i].ea, list[i].size);
        d += list[i].size;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

void vxio_wait_us(uint32_t us)
{
    usleep(us < 20 ? 20 : us);
}

static void* thread_main(void* arg)
{
    vx_spu_main((uint32_t) (uintptr_t) arg);
    return NULL;
}

bool vx_start(VxShared* shared)
{
    pthread_t t;

    return pthread_create(&t, NULL, thread_main, shared) == 0;
}
