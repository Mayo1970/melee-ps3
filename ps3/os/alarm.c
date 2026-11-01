// OS alarms on one lv2 thread. A handler runs with interrupts disabled, as from the GameCube decrementer, and
// the calls that change alarms disable interrupts too, so a cancelled alarm never fires afterwards.
#include <stdbool.h>

#include <dolphin/os.h>

#include "alarm.h"
#include "diag.h"
#include "intr.h"
#include "sync.h"

static PsMutex lock = PS_MUTEX_INIT;
static PsCond cond = PS_COND_INIT;
// Sorted by fire time.
static OSAlarm* head;
static OSAlarm* tail;

// The caller holds lock.
static void dequeue(OSAlarm* alarm)
{
    if (alarm->next != NULL) {
        alarm->next->prev = alarm->prev;
    } else {
        tail = alarm->prev;
    }
    if (alarm->prev != NULL) {
        alarm->prev->next = alarm->next;
    } else {
        head = alarm->next;
    }
    alarm->prev = alarm->next = NULL;
}

// The caller holds lock.
static void enqueue(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler)
{
    OSAlarm* after = tail;

    alarm->fire = fire;
    alarm->handler = handler;
    while (after != NULL && after->fire > fire) {
        after = after->prev;
    }
    alarm->prev = after;
    alarm->next = after != NULL ? after->next : head;
    if (alarm->next != NULL) {
        alarm->next->prev = alarm;
    } else {
        tail = alarm;
    }
    if (after != NULL) {
        after->next = alarm;
    } else {
        head = alarm;
        ps_cond_signal(&cond);
    }
}

static bool queued(OSAlarm* alarm)
{
    return alarm->prev != NULL || head == alarm;
}

// Takes the first alarm that is due, with a periodic one queued again for its next period.
static OSAlarm* take_due(OSTime now, OSAlarmHandler* handler)
{
    OSAlarm* alarm;

    ps_mutex_lock(&lock);
    alarm = head;
    if (alarm == NULL || alarm->fire > now) {
        ps_mutex_unlock(&lock);
        return NULL;
    }
    *handler = alarm->handler;
    dequeue(alarm);
    if (alarm->period > 0) {
        enqueue(alarm, alarm->start + ((now - alarm->start) / alarm->period + 1) * alarm->period, alarm->handler);
    } else {
        alarm->handler = NULL;
    }
    ps_mutex_unlock(&lock);
    return alarm;
}

static void alarm_main(void* arg)
{
    for (;;) {
        OSAlarmHandler handler;
        OSAlarm* alarm;
        BOOL enabled;

        ps_mutex_lock(&lock);
        for (;;) {
            OSTime now = OSGetTime();

            if (head == NULL) {
                ps_cond_wait(&cond, &lock);
            } else if (head->fire > now) {
                ps_cond_timedwait(&cond, &lock, (unsigned long long) OSTicksToMicroseconds(head->fire - now) + 1);
            } else {
                break;
            }
        }
        ps_mutex_unlock(&lock);
        enabled = OSDisableInterrupts();
        while ((alarm = take_due(OSGetTime(), &handler)) != NULL) {
            ps3_diag.alarms++;
            handler(alarm, NULL);
        }
        ps3_intr_alarms_ran();
        OSRestoreInterrupts(enabled);
    }
}

void OSInitAlarm(void)
{
    static bool started;

    if (!started) {
        started = true;
        if (ps_thread_start(alarm_main, NULL, "alarms") != 0) {
            OSReport("OSInitAlarm: the alarm thread did not start\n");
        }
    }
}

void OSCreateAlarm(OSAlarm* alarm)
{
    alarm->handler = NULL;
    alarm->tag = 0;
    alarm->prev = alarm->next = NULL;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
{
    BOOL enabled = OSDisableInterrupts();

    ps_mutex_lock(&lock);
    if (queued(alarm)) {
        dequeue(alarm);
    }
    alarm->period = 0;
    enqueue(alarm, OSGetTime() + tick, handler);
    ps_mutex_unlock(&lock);
    OSRestoreInterrupts(enabled);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler)
{
    BOOL enabled = OSDisableInterrupts();
    OSTime now = OSGetTime();
    OSTime fire = start;

    ps_mutex_lock(&lock);
    if (queued(alarm)) {
        dequeue(alarm);
    }
    alarm->period = period;
    alarm->start = start;
    // A start in the past fires at the next period boundary after now, as in the SDK.
    if (fire <= now && period > 0) {
        fire = start + ((now - start) / period + 1) * period;
    }
    enqueue(alarm, fire, handler);
    ps_mutex_unlock(&lock);
    OSRestoreInterrupts(enabled);
}

void ps3_alarm_foreach(void (*fn)(OSAlarm* alarm, void* arg), void* arg)
{
    OSAlarm* alarm;

    ps_mutex_lock(&lock);
    for (alarm = head; alarm != NULL; alarm = alarm->next) {
        fn(alarm, arg);
    }
    ps_mutex_unlock(&lock);
}

void OSCancelAlarm(OSAlarm* alarm)
{
    BOOL enabled = OSDisableInterrupts();

    ps_mutex_lock(&lock);
    if (queued(alarm)) {
        dequeue(alarm);
    }
    alarm->handler = NULL;
    ps_mutex_unlock(&lock);
    OSRestoreInterrupts(enabled);
}
