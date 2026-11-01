// What the game needs from the port's interrupts (ps3/os/intr.c), under TARGET_PS3.
#ifndef PS3_INTR_H
#define PS3_INTR_H

// With interrupts disabled, after checking what an alarm handler changes: sleeps with them enabled until the
// alarm thread has run alarms or usec have passed. The caller checks again, as after OSSleepThread.
void ps3_intr_wait_alarms(unsigned long long usec);

#endif
