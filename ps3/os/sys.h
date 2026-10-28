// The lv2 side of the OS layer: process setup, time, free memory and the XMB. Standard C types only, so
// SDK sources can include it.
#ifndef PS3_OS_SYS_H
#define PS3_OS_SYS_H

#include <stdbool.h>

// Starts the UDP log and the XMB event handling, and logs the free memory. Call first.
void ps3_sys_init(void);

// The PPE time base (it never reads 0 in its low word) and its frequency in Hz.
unsigned long long ps3_sys_timebase(void);
unsigned long long ps3_sys_timebase_freq(void);

// The console's local time, as seconds since 1970-01-01.
long long ps3_sys_local_time(void);

// The largest memory container lv2 grants now, in MB: the free user memory.
unsigned ps3_sys_free_mb(void);

// Runs the XMB callbacks. Returns true once the XMB has asked the game to quit.
bool ps3_sys_poll(void);

// Ends the process and returns to the XMB.
void ps3_sys_exit(void) __attribute__((noreturn));

#endif
