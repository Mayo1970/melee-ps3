// OS threads on lv2 threads. Only the crash console creates one, and GX keeps the current thread for FIFO
// overflows, which the port's ring never raises. Thread queues are in intr.c.
#include <stdbool.h>

#include <dolphin/os.h>

#include "sync.h"

#define MAX_THREADS 4

typedef struct Created {
    OSThread* thread;
    void* (*func)(void*);
    void* param;
} Created;

static OSThread main_thread;
static __thread OSThread* current;
static Created created[MAX_THREADS];
static int created_count;

OSThread* OSGetCurrentThread(void)
{
    return current != NULL ? current : &main_thread;
}

static void entry(void* arg)
{
    Created* c = arg;

    current = c->thread;
    c->thread->state = OS_THREAD_STATE_RUNNING;
    c->thread->val = c->func(c->param);
    c->thread->state = OS_THREAD_STATE_MORIBUND;
}

static Created* find(OSThread* thread)
{
    int i;

    for (i = 0; i < created_count; i++) {
        if (created[i].thread == thread) {
            return &created[i];
        }
    }
    return NULL;
}

// The thread runs on its own lv2 stack; stack and priority are the GameCube's.
int OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, unsigned long stackSize,
                   long priority, unsigned short attr)
{
    Created* c = find(thread);

    if (c == NULL) {
        if (created_count == MAX_THREADS) {
            OSReport("OSCreateThread: more than %d threads\n", MAX_THREADS);
            return FALSE;
        }
        c = &created[created_count++];
    }
    c->thread = thread;
    c->func = func;
    c->param = param;
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attr;
    thread->suspend = 1;
    thread->priority = thread->base = priority;
    thread->val = NULL;
    return TRUE;
}

s32 OSResumeThread(OSThread* thread)
{
    s32 old = thread->suspend;
    Created* c;

    if (old > 0 && --thread->suspend == 0 && thread->state == OS_THREAD_STATE_READY &&
        (c = find(thread)) != NULL && ps_thread_start(entry, c, "OSThread") != 0)
    {
        OSReport("OSResumeThread: the thread did not start\n");
    }
    return old;
}

// An lv2 thread cannot be stopped from outside; the count is kept for the callers that read it back.
s32 OSSuspendThread(OSThread* thread)
{
    return thread->suspend++;
}

long OSCheckActiveThreads(void)
{
    return 1 + created_count;
}
