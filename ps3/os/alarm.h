#ifndef PS3_OS_ALARM_H
#define PS3_OS_ALARM_H

#include <dolphin/os.h>

// Calls fn for each queued alarm, under the alarm lock: their links belong to the alarm thread.
void ps3_alarm_foreach(void (*fn)(OSAlarm* alarm, void* arg), void* arg);

#endif
