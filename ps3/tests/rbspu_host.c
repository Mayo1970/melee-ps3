// Host stand-ins for the rollback's SPU program: a thread instead of an SPU, DMA as memcpy. Addresses below 4 GB, as
// under qemu-ppc64 -R 0x100000000.
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "../net/rb_shared.h"
#include "../net/rbspu_io.h"

bool rb_start(RbShared* shared);

void rbio_get(void* ls, uint32_t ea, uint32_t size)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    memcpy(ls, (const void*) (uintptr_t) ea, size);
}

void rbio_put(const void* ls, uint32_t ea, uint32_t size)
{
    memcpy((void*) (uintptr_t) ea, ls, size);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

void rbio_wait(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void rbio_wait_us(uint32_t us)
{
    usleep(us < 20 ? 20 : us);
}

static void* thread_main(void* arg)
{
    rb_spu_main((uint32_t) (uintptr_t) arg);
    return NULL;
}

bool rb_start(RbShared* shared)
{
    pthread_t t;

    return pthread_create(&t, NULL, thread_main, shared) == 0;
}
