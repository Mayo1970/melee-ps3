// Counters and states that the VI heartbeat logs once a second (step 3 diagnostics). Defined in intr.c.
#ifndef PS3_OS_DIAG_H
#define PS3_OS_DIAG_H

#define PS3_CALLTRACE_DEPTH 64
#define PS3_CALLTRACE_RECENT 8

typedef struct Ps3Diag {
    // OSDisableInterrupts calls made by the main thread.
    volatile unsigned main_disables;
    volatile unsigned alarms, pad_reads, retrace_waits, card_calls;
    volatile unsigned dvd_reads, dvd_callbacks, arq_posts, arq_callbacks;
    // The thread that holds the interrupt lock, by name; NULL when none does.
    const char* volatile intr_owner;
    // The handler step that runs, by name; NULL outside the handlers.
    const char* volatile intr_site;
    // What the GP thread waits for: 0 nothing, 1 ring data, 2 a display flip, 3 an RSX label.
    volatile unsigned gp_wait;
    // The GP thread's step (a name), the last opcode it read, and the backend's last draw: where it stopped.
    const char* volatile gp_site;
    volatile unsigned gp_op, gp_draw;
    // The thread queue the main thread sleeps on (OSSleepThread), NULL while it runs.
    const void* volatile main_sleep;
    // 1 while the GX producer waits for ring space.
    volatile unsigned ring_space_wait;
    volatile unsigned vblanks;
    // Connected pads, and the first one's PS3_PAD_* buttons (ps3/pad/pad_cell.h).
    volatile unsigned pads, pad0_buttons;
    // A/B (Select): pad.c names its modes (2^26, 2^27 the GP's). Older: 1 no batching/list cache, 1024 no list cache,
    // 2 VP reloads, 4 all consts, 8/32 wait/flush a draw, 64/128/256 barrier before consts/VPs/texs, 512 present wait.
    volatile unsigned gx_ab;
    // Set by the RPCS3 build's automatic press: log each interrupt taken.
    volatile unsigned trace;
    // Call trace build only (calltrace.c): the main thread's function entries and its open calls, innermost last.
    volatile unsigned main_calls, main_depth;
    void* volatile main_stack[PS3_CALLTRACE_DEPTH];
    void* volatile main_recent[PS3_CALLTRACE_RECENT];
    // A software watchpoint: the first traced call, on any thread, that sees *watch differ from watch_value.
    // watch_site is in the function that ran when the word changed; the main stack is copied at that call.
    unsigned long long* volatile watch;
    unsigned long long watch_value, watch_seen;
    void* volatile watch_fn;
    void* volatile watch_site;
    volatile unsigned watch_hit, watch_on_main, watch_depth;
    void* volatile watch_stack[PS3_CALLTRACE_DEPTH];
} Ps3Diag;

extern Ps3Diag ps3_diag;

// Weak: the host tests link intr.c without calltrace.c.
void ps3_calltrace_main(void) __attribute__((weak));

#endif
