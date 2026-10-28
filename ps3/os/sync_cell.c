// The port's sync objects on lv2, after DolphinPS3's ps3/compat/ps3_threads.cpp. See sync.h.
#include "sync.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/cond.h>
#include <lv2/mutex.h>
#include <sys/thread.h>

#define THREAD_PRIORITY 1001
#define THREAD_STACK_SIZE (256 * 1024)

typedef char lwmutex_fits[sizeof(sys_lwmutex_t) <= sizeof(((PsMutex*) 0)->storage) ? 1 : -1];
typedef char lwcond_fits[sizeof(sys_lwcond_t) <= sizeof(((PsCond*) 0)->storage) ? 1 : -1];

// State 0: not created; 1: being created; 2: ready.
static bool claim(volatile unsigned* state)
{
    unsigned expected = 0;

    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 2) {
        return false;
    }
    if (__atomic_compare_exchange_n(state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return true;
    }
    // Another thread creates it.
    while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2) {
        sysThreadYield();
    }
    return false;
}

static sys_lwmutex_t* lwmutex(PsMutex* m)
{
    sys_lwmutex_t* lw = (sys_lwmutex_t*) m->storage;

    if (claim(&m->state)) {
        sys_lwmutex_attr_t attr;

        memset(&attr, 0, sizeof(attr));
        attr.attr_protocol = SYS_LWMUTEX_PROTOCOL_PRIO;
        attr.attr_recursive = SYS_LWMUTEX_ATTR_NOT_RECURSIVE;
        strncpy(attr.name, "melee", sizeof(attr.name));
        if (sysLwMutexCreate(lw, &attr) != 0) {
            abort();
        }
        __atomic_store_n(&m->state, 2, __ATOMIC_RELEASE);
    }
    return lw;
}

void ps_mutex_lock(PsMutex* m)
{
    sysLwMutexLock(lwmutex(m), 0);
}

void ps_mutex_unlock(PsMutex* m)
{
    sysLwMutexUnlock((sys_lwmutex_t*) m->storage);
}

// timeout in microseconds, 0 for none.
static void cond_wait(PsCond* c, PsMutex* m, u64 timeout)
{
    sys_lwcond_t* lw = (sys_lwcond_t*) c->storage;

    if (claim(&c->state)) {
        sys_lwcond_attr_t attr;

        memset(&attr, 0, sizeof(attr));
        strncpy(attr.name, "melee", sizeof(attr.name));
        if (sysLwCondCreate(lw, lwmutex(m), &attr) != 0) {
            abort();
        }
        __atomic_store_n(&c->state, 2, __ATOMIC_RELEASE);
    }
    sysLwCondWait(lw, timeout);
}

void ps_cond_wait(PsCond* c, PsMutex* m)
{
    cond_wait(c, m, 0);
}

void ps_cond_timedwait(PsCond* c, PsMutex* m, unsigned long long usec)
{
    cond_wait(c, m, usec != 0 ? usec : 1);
}

void ps_cond_signal(PsCond* c)
{
    // Not created yet means nobody has waited, so there is nobody to wake.
    if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == 2) {
        sysLwCondSignal((sys_lwcond_t*) c->storage);
    }
}

void ps_cond_broadcast(PsCond* c)
{
    if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == 2) {
        sysLwCondSignalAll((sys_lwcond_t*) c->storage);
    }
}

typedef struct Start {
    void (*fn)(void*);
    void* arg;
    const char* name;
} Start;

static __thread const char* thread_name;

const char* ps_thread_name(void)
{
    return thread_name;
}

static void entry(void* arg)
{
    Start s = *(Start*) arg;

    free(arg);
    thread_name = s.name;
    s.fn(s.arg);
    sysThreadExit(0);
}

int ps_thread_start(void (*fn)(void* arg), void* arg, const char* name)
{
    Start* s = malloc(sizeof(*s));
    sys_ppu_thread_t tid;
    char buf[28];
    int err;

    if (s == NULL) {
        return -1;
    }
    s->fn = fn;
    s->arg = arg;
    s->name = name;
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    err = sysThreadCreate(&tid, entry, s, THREAD_PRIORITY, THREAD_STACK_SIZE, 0, buf);
    if (err != 0) {
        free(s);
    }
    return err;
}
