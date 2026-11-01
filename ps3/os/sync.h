// Mutexes, condition variables and threads for the port. On the PS3 they are lv2 lwmutexes, lwconds and PPU
// threads, as DolphinPS3's ps3_threads: PSL1GHT's PTE pthread waits spin on sys_semaphore_trywait.
#ifndef PS3_OS_SYNC_H
#define PS3_OS_SYNC_H

// Standard C types only, so SDK sources can include it. Zeroed objects are valid; each is created on first use.
typedef struct PsMutex {
    unsigned long long storage[8];
    volatile unsigned state;
} PsMutex;

// The first wait binds it to its mutex, as an lv2 lwcond is.
typedef struct PsCond {
    unsigned long long storage[8];
    volatile unsigned state;
} PsCond;

#define PS_MUTEX_INIT { { 0 }, 0 }
#define PS_COND_INIT { { 0 }, 0 }

void ps_mutex_lock(PsMutex* m);
void ps_mutex_unlock(PsMutex* m);
// The caller holds m; it holds it again on return.
void ps_cond_wait(PsCond* c, PsMutex* m);
// As ps_cond_wait, for at most usec microseconds; a wakeup may also come early, so recheck the condition.
void ps_cond_timedwait(PsCond* c, PsMutex* m, unsigned long long usec);
void ps_cond_signal(PsCond* c);
void ps_cond_broadcast(PsCond* c);
// A detached thread running fn(arg). Returns 0, or an error code.
int ps_thread_start(void (*fn)(void* arg), void* arg, const char* name);
// The name ps_thread_start gave the calling thread; NULL for other threads.
const char* ps_thread_name(void);
// The calling thread's lv2 priority (lower runs first; ps_thread_start gives 1001, the main thread has 1100).
// Returns 0, or an error code; nothing on the host.
int ps_thread_set_priority(int priority);

#endif
