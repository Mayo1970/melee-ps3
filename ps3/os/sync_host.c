// The port's sync objects on pthreads, for the host tests. See sync.h.
#include "sync.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef char mutex_fits[sizeof(pthread_mutex_t) <= sizeof(((PsMutex*) 0)->storage) ? 1 : -1];
typedef char cond_fits[sizeof(pthread_cond_t) <= sizeof(((PsCond*) 0)->storage) ? 1 : -1];

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
    while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2) {
        sched_yield();
    }
    return false;
}

static pthread_mutex_t* mutex(PsMutex* m)
{
    if (claim(&m->state)) {
        pthread_mutex_init((pthread_mutex_t*) m->storage, NULL);
        __atomic_store_n(&m->state, 2, __ATOMIC_RELEASE);
    }
    return (pthread_mutex_t*) m->storage;
}

static pthread_cond_t* cond(PsCond* c)
{
    if (claim(&c->state)) {
        pthread_cond_init((pthread_cond_t*) c->storage, NULL);
        __atomic_store_n(&c->state, 2, __ATOMIC_RELEASE);
    }
    return (pthread_cond_t*) c->storage;
}

void ps_mutex_lock(PsMutex* m)
{
    pthread_mutex_lock(mutex(m));
}

void ps_mutex_unlock(PsMutex* m)
{
    pthread_mutex_unlock(mutex(m));
}

void ps_cond_wait(PsCond* c, PsMutex* m)
{
    pthread_cond_wait(cond(c), mutex(m));
}

void ps_cond_timedwait(PsCond* c, PsMutex* m, unsigned long long usec)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t) (usec / 1000000);
    ts.tv_nsec += (long) (usec % 1000000) * 1000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(cond(c), mutex(m), &ts);
}

void ps_cond_signal(PsCond* c)
{
    pthread_cond_signal(cond(c));
}

void ps_cond_broadcast(PsCond* c)
{
    pthread_cond_broadcast(cond(c));
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

static void* entry(void* arg)
{
    Start s = *(Start*) arg;

    free(arg);
    thread_name = s.name;
    s.fn(s.arg);
    return NULL;
}

int ps_thread_start(void (*fn)(void* arg), void* arg, const char* name)
{
    Start* s = malloc(sizeof(*s));
    pthread_t t;
    int err;

    (void) name;
    if (s == NULL) {
        return -1;
    }
    s->fn = fn;
    s->arg = arg;
    s->name = name;
    err = pthread_create(&t, NULL, entry, s);
    if (err != 0) {
        free(s);
        return err;
    }
    pthread_detach(t);
    return 0;
}
