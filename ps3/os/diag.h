// Counters and states that the VI heartbeat logs once a second (step 3 diagnostics). Defined in intr.c.
#ifndef PS3_OS_DIAG_H
#define PS3_OS_DIAG_H

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
    // 1 while the GX producer waits for ring space.
    volatile unsigned ring_space_wait;
    volatile unsigned vblanks;
    // Connected pads, and the first one's PS3_PAD_* buttons (ps3/pad/pad_cell.h).
    volatile unsigned pads, pad0_buttons;
    // Set by the RPCS3 build's automatic press: log each interrupt taken.
    volatile unsigned trace;
} Ps3Diag;

extern Ps3Diag ps3_diag;

#endif
