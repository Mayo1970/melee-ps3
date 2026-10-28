#ifndef PS3_OS_INTR_H
#define PS3_OS_INTR_H

// Interrupt sources, served in this order, each handler with interrupts disabled. DSP before AI: AX sees the
// DSP finish a frame before the next AI period. VI after PE: a draw done raised with a retrace goes first.
enum {
    PS3_INTR_DVD,
    PS3_INTR_ARAM,
    PS3_INTR_DSP,
    PS3_INTR_AI,
    PS3_INTR_PE_TOKEN,
    PS3_INTR_PE_FINISH,
    PS3_INTR_VI,
    PS3_INTR_COUNT,
};

typedef void (*Ps3IntrHandler)(void);

// Starts the thread that takes raised interrupts while no game thread enables interrupts.
void ps3_intr_init(void);
void ps3_intr_set_handler(int source, Ps3IntrHandler handler);
// Any thread may raise; the handler runs on the next thread that enables interrupts, or on the
// interrupt thread, whichever comes first.
void ps3_intr_raise(int source);

#endif
