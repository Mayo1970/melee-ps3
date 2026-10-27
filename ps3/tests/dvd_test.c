// Host test of ps3/dvd. Reads each file listed on stdin as devcom does and prints "path size crc32",
// to diff against an independent reader. Run it under qemu-ppc64 for the PS3's byte order and LP64.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include "../dvd/dvd.h"

#define CHUNK 0x80000

static u32 crc_table[256];
static int done;
static s32 done_result;

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

static void on_read(s32 result, DVDFileInfo* file)
{
    done++;
    done_result = result;
}

// The same entry for the path without its leading slash, through "./", and in upper case.
static int check_lookups(const char* path, s32 entry)
{
    char alt[1100];
    int failures = 0;
    size_t i;

    failures += DVDConvertPathToEntrynum(path + 1) != entry;
    snprintf(alt, sizeof(alt), "/./%s", path + 1);
    failures += DVDConvertPathToEntrynum(alt) != entry;
    for (i = 0; path[i] != '\0' && i + 1 < sizeof(alt); i++) {
        alt[i] = (path[i] >= 'a' && path[i] <= 'z') ? path[i] - 'a' + 'A' : path[i];
    }
    alt[i] = '\0';
    failures += DVDConvertPathToEntrynum(alt) != entry;
    if (failures != 0) {
        fprintf(stderr, "%s: %d lookups gave another entry\n", path, failures);
    }
    return failures;
}

int main(int argc, char** argv)
{
    static u8 buf[CHUNK] __attribute__((aligned(32)));
    char line[1024];
    char path[1024];
    int files = 0;
    int failures = 0;
    bool mounted;
    DVDDiskID* id;

    mounted = argc > 1 && strcmp(argv[1], "--default") != 0 ? ps3_dvd_mount(argv[1])
                                                            : ps3_dvd_mount_default();
    if (!mounted) {
        return 1;
    }
    id = DVDGetCurrentDiskID();
    fprintf(stderr, "disc %.4s%.2s revision %d\n", id->gameName, id->company, id->gameVersion);
    crc_init();

    while (fgets(line, sizeof(line), stdin) != NULL) {
        DVDFileInfo info;
        s32 entry;
        u32 crc = 0;
        u32 off;

        if (sscanf(line, "%1023s", path) != 1) {
            continue;
        }
        entry = DVDConvertPathToEntrynum(path);
        if (entry < 0 || !DVDFastOpen(entry, &info)) {
            printf("%s missing\n", path);
            failures++;
            continue;
        }
        failures += check_lookups(path, entry);
        for (off = 0; off < info.length; off += CHUNK) {
            u32 left = info.length - off;
            u32 len = left < CHUNK ? (left + 31) & ~31u : CHUNK;

            done = 0;
            if (!DVDReadAsyncPrio(&info, buf, (s32) len, (s32) off, on_read, 2)) {
                fprintf(stderr, "%s: read at %u refused\n", path, off);
                failures++;
                break;
            }
            if (done != 0) {
                fprintf(stderr, "%s: callback ran inside the read\n", path);
                failures++;
            }
            ps3_dvd_dispatch();
            if (done != 1 || done_result != (s32) len) {
                fprintf(stderr, "%s: read at %u gave %d\n", path, off, (int) done_result);
                failures++;
                break;
            }
            crc = crc_update(crc, buf, left < len ? left : len);
        }
        DVDClose(&info);
        printf("%s %u %08x\n", path, info.length, crc);
        files++;
    }
    failures += DVDConvertPathToEntrynum("/no such file.dat") != -1;
    failures += DVDConvertPathToEntrynum("/../") != 0;
    fprintf(stderr, "%d files, %d failures\n", files, failures);
    return failures != 0;
}
