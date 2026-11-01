// The lv2 side of the OS layer for the host game build (see sys.h), and the build's settings.
#include "../os/sys.h"
#include "../os/udplog.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

void soft_set_dump(const char* prefix);
void soft_set_dump_every(uint32_t n);
extern bool soft_drop, soft_sparse;
extern uint32_t soft_dump_from;

static double quit_at;
static pthread_t main_thread;
static int exit_code = 1;

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec * 1e-9;
}

// A fault logs where it happened and the back chain's return addresses, as OSPanic does, for nm on melee_host.
// SIGUSR1 logs the main thread's place at the quit, and returns.
static void on_fault(int sig, siginfo_t* info, void* context)
{
    const ucontext_t* uc = context;
    const uint64_t* frame = (const uint64_t*) uc->uc_mcontext.gp_regs[1];
    int i;

    ps3_log("melee: %s %d at %#llx, address %p, LR %#llx; stack:",
            sig == SIGUSR1 ? "main thread at quit, signal" : "signal", sig,
            (unsigned long long) uc->uc_mcontext.gp_regs[32], info->si_addr,
            (unsigned long long) uc->uc_mcontext.gp_regs[36]);
    for (i = 0; i < 24 && frame != NULL && ((uintptr_t) frame & 7) == 0; i++) {
        const uint64_t* next = (const uint64_t*) frame[0];

        if (next <= frame || (uintptr_t) next - (uintptr_t) frame > 0x100000) {
            break;
        }
        ps3_log(" %#llx", (unsigned long long) next[2]);
        frame = next;
    }
    ps3_log("\n");
    fflush(stdout);
    if (sig != SIGUSR1) {
        _exit(2);
    }
}

// MELEE_HOST_SECONDS: quit after that long. MELEE_HOST_DRAW=1: draw in software, 2: only dumped frames.
// MELEE_HOST_DUMP=prefix: every MELEE_HOST_DUMP_EVERY-th present (default 60), from MELEE_HOST_DUMP_FROM, to PPM.
void ps3_sys_init(void)
{
    const char* seconds = getenv("MELEE_HOST_SECONDS");
    const char* draw = getenv("MELEE_HOST_DRAW");
    const char* dump = getenv("MELEE_HOST_DUMP");
    const char* every = getenv("MELEE_HOST_DUMP_EVERY");

    struct sigaction sa;

    ps3_log_init();
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    // glibc's buffer overflow checks abort: the back chain names the caller.
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    main_thread = pthread_self();
    ps3_log("melee: host boot, %u MB free\n", ps3_sys_free_mb());
    if (seconds != NULL) {
        quit_at = now() + atof(seconds);
    }
    soft_drop = draw == NULL || draw[0] != '1';
    soft_sparse = draw != NULL && draw[0] == '2';
    if (dump != NULL) {
        soft_set_dump(dump);
        soft_set_dump_every(every != NULL ? (uint32_t) atoi(every) : 60);
        soft_dump_from = getenv("MELEE_HOST_DUMP_FROM") != NULL ? (uint32_t) atoi(getenv("MELEE_HOST_DUMP_FROM")) : 0;
    }
}

unsigned long long ps3_sys_timebase(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    // + 1: the PPE time base never reads 0 in its low word, so neither does this.
    return (unsigned long long) t.tv_sec * 1000000000ull + (unsigned long long) t.tv_nsec + 1;
}

unsigned long long ps3_sys_timebase_freq(void)
{
    return 1000000000ull;
}

long long ps3_sys_local_time(void)
{
    time_t t = time(NULL);
    struct tm tm;

    localtime_r(&t, &tm);
    return (long long) t + tm.tm_gmtoff;
}

unsigned ps3_sys_free_mb(void)
{
    long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);

    return pages > 0 && size > 0 ? (unsigned) ((unsigned long long) pages * (unsigned long long) size >> 20) : 0;
}

bool ps3_sys_poll(void)
{
    if (quit_at != 0 && now() >= quit_at) {
        exit_code = 0;
        return true;
    }
    return false;
}

// No XMB on the host.
bool ps3_sys_overlay(void)
{
    return false;
}

// Exit status 0 only for the MELEE_HOST_SECONDS quit; a panic or a reset gives 1.
void ps3_sys_exit(void)
{
    if (exit_code == 0 && !pthread_equal(pthread_self(), main_thread)) {
        pthread_kill(main_thread, SIGUSR1);
        usleep(300000);
    }
    ps3_log("melee: exit (%d)\n", exit_code);
    fflush(stdout);
    exit(exit_code);
}
