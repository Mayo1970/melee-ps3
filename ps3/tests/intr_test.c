// Host test of ps3/os/intr with ps3/dvd. Reads every listed file through a copy of devcom's request
// chain and checks that no callback runs inside a disabled section. Prints "path size crc32" lines.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include "../dvd/dvd.h"
#include "../os/intr.h"

#define CHUNK 0x80000
#define MAX_FILES 2048
#define STALL_SECONDS 60

typedef struct Request {
    char path[256];
    s32 entry;
    u32 size;
    u32 done;
    u32 crc;
} Request;

static Request reqs[MAX_FILES];
static int nreqs;
static int next_req;
static Request* cur;
static volatile int completed;
static DVDFileInfo fileinfo;
static bool busy;
static u8 buf[CHUNK] __attribute__((aligned(32)));
static u32 last_len;

static pthread_t main_thread;
static volatile bool main_in_section;
static int on_main, on_other, in_section;

static u32 crc_table[256];

void OSReport(char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void crc_init(void)
{
    u32 i;
    for (i = 0; i < 256; i++) {
        u32 c = i;
        int k;
        for (k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        }
        crc_table[i] = c;
    }
}

static u32 crc_update(u32 crc, const u8* p, size_t n)
{
    crc = ~crc;
    while (n-- != 0) {
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static void note_callback(void)
{
    if (main_in_section) {
        in_section++;
    }
    if (pthread_equal(pthread_self(), main_thread)) {
        on_main++;
    } else {
        on_other++;
    }
}

static void wake_up(void);

static void on_chunk(s32 result, DVDFileInfo* file)
{
    u32 left = cur->size - cur->done;

    note_callback();
    if (result != (s32) last_len) {
        fprintf(stderr, "%s: read gave %d\n", cur->path, (int) result);
    }
    cur->crc = crc_update(cur->crc, buf, left < last_len ? left : last_len);
    cur->done += left < last_len ? left : last_len;
    if (cur->done == cur->size) {
        cur = NULL;
        completed++;
    }
    busy = false;
    wake_up();
}

// devcom's HSD_DevComDVDWakeUp: it marks itself busy only after the read call returns.
static void wake_up(void)
{
    BOOL enabled = OSDisableInterrupts();
    u32 left;

    if (enabled) {
        main_in_section = pthread_equal(pthread_self(), main_thread);
    }
    if (!busy && cur == NULL && next_req < nreqs) {
        cur = &reqs[next_req++];
    }
    if (!busy && cur != NULL) {
        left = cur->size - cur->done;
        last_len = left < CHUNK ? (left + 31) & ~31u : CHUNK;
        DVDFastOpen(cur->entry, &fileinfo);
        DVDReadAsyncPrio(&fileinfo, buf, (s32) last_len, (s32) cur->done, on_chunk, 2);
        busy = true;
    }
    if (enabled) {
        main_in_section = false;
    }
    OSRestoreInterrupts(enabled);
}

static volatile int ax_flag;

static void on_ax_read(s32 result, DVDFileInfo* file)
{
    note_callback();
    ax_flag = result;
}

static bool wait_for(volatile int* value, int target, const char* what)
{
    time_t start = time(NULL);

    while (*value < target) {
        // Interrupts stay enabled and are never touched here, so only the interrupt thread delivers.
        usleep(1000);
        if (time(NULL) - start > STALL_SECONDS) {
            fprintf(stderr, "%s stalled at %d of %d\n", what, *value, target);
            return false;
        }
    }
    return true;
}

int main(void)
{
    char line[512];
    int failures = 0;
    int before;
    int i;

    main_thread = pthread_self();
    crc_init();
    ps3_intr_init();
    DVDInit();
    if (!DVDCheckDisk()) {
        return 1;
    }
    while (nreqs < MAX_FILES && fgets(line, sizeof(line), stdin) != NULL) {
        Request* r = &reqs[nreqs];
        DVDFileInfo info;
        if (sscanf(line, "%255s", r->path) != 1) {
            continue;
        }
        r->entry = DVDConvertPathToEntrynum(r->path);
        if (r->entry < 0 || !DVDFastOpen(r->entry, &info)) {
            fprintf(stderr, "%s missing\n", r->path);
            failures++;
            continue;
        }
        r->size = info.length;
        nreqs++;
    }

    // devcom's chain, started from the main thread as HSD_DevComRequest starts it.
    wake_up();
    failures += !wait_for(&completed, nreqs, "devcom chain");
    for (i = 0; i < nreqs; i++) {
        printf("%s %u %08x\n", reqs[i].path, reqs[i].size, reqs[i].crc);
    }

    // AXDriver_8038DA70: one read, then a wait that never enables interrupts.
    before = on_other;
    {
        DVDFileInfo info;
        DVDFastOpen(reqs[0].entry, &info);
        ax_flag = 0;
        DVDReadAsyncPrio(&info, buf, (s32) ((reqs[0].size + 31) & ~31u), 0, on_ax_read, 2);
        failures += !wait_for(&ax_flag, 1, "AXDriver wait");
    }
    if (on_other == before) {
        fprintf(stderr, "the interrupt thread did not deliver the AXDriver read\n");
        failures++;
    }

    fprintf(stderr, "%d files; callbacks: %d on the main thread, %d on the interrupt thread, "
                    "%d inside a disabled section; %d failures\n",
            completed, on_main, on_other, in_section, failures + in_section);
    return failures + in_section != 0;
}
