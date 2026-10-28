// The main thread's calls for the heartbeat, when the game is built with -finstrument-functions (step 3
// diagnostics). The addresses are the functions' .opd descriptors, as ppu-nm names them.
#include <stddef.h>

#include "diag.h"

#define NO_TRACE __attribute__((no_instrument_function))

static __thread int on_main;

void ps3_calltrace_main(void) NO_TRACE;
void __cyg_profile_func_enter(void* fn, void* site) NO_TRACE;
void __cyg_profile_func_exit(void* fn, void* site) NO_TRACE;

void ps3_calltrace_main(void)
{
    on_main = 1;
}

static void check_watch(void* fn, void* site) NO_TRACE;

static void check_watch(void* fn, void* site)
{
    unsigned long long* watch = ps3_diag.watch;
    unsigned i;

    if (watch == NULL || ps3_diag.watch_hit || *watch == ps3_diag.watch_value) {
        return;
    }
    ps3_diag.watch_hit = 1;
    ps3_diag.watch_seen = *watch;
    ps3_diag.watch_fn = fn;
    ps3_diag.watch_site = site;
    ps3_diag.watch_on_main = on_main;
    ps3_diag.watch_depth = ps3_diag.main_depth;
    for (i = 0; i < PS3_CALLTRACE_DEPTH; i++) {
        ps3_diag.watch_stack[i] = ps3_diag.main_stack[i];
    }
}

void __cyg_profile_func_enter(void* fn, void* site)
{
    unsigned depth;

    check_watch(fn, site);
    if (!on_main) {
        return;
    }
    depth = ps3_diag.main_depth;
    if (depth < PS3_CALLTRACE_DEPTH) {
        ps3_diag.main_stack[depth] = fn;
    }
    ps3_diag.main_depth = depth + 1;
    ps3_diag.main_recent[ps3_diag.main_calls % PS3_CALLTRACE_RECENT] = fn;
    ps3_diag.main_calls++;
}

void __cyg_profile_func_exit(void* fn, void* site)
{
    check_watch(fn, site);
    if (on_main && ps3_diag.main_depth != 0) {
        ps3_diag.main_depth--;
    }
}
