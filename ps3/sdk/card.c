// The memory card: a Memory Card 251 in slot A, its files as GCI files (the 64-byte directory entry, then the
// data, as Dolphin exports them) in a folder on the HDD. Slot B is empty. Callbacks come as EXI interrupts.
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <dolphin/card.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include "../os/diag.h"
#include "../os/intr.h"
#include "../os/udplog.h"

#ifndef PS3_CARD_DIR
#define PS3_CARD_DIR "/dev_hdd0/game/MELE00001/USRDIR/card"
#endif

#define SECTOR 8192
// 16 Mbit: 256 blocks, 5 of them the card's own.
#define MEM_MBITS 16
#define USER_BLOCKS 251
#define DIR_SIZE 64
#define ATTR_PUBLIC 0x04
#define BANNER_W 96
#define BANNER_H 32
#define ICON_W 32
#define ICON_H 32
#define MAX_PENDING 16

typedef struct File {
    bool used;
    CARDDir dir;
    u8* data;
    char path[256];
} File;

static File files[CARD_MAX_FILE];
static bool mounted;
static s32 last_result;
static long xferred;

static struct {
    CARDCallback callback;
    s32 chan, result;
} pending[MAX_PENDING];
static u32 pending_head, pending_tail;

static void dispatch(void)
{
    for (;;) {
        BOOL enabled = OSDisableInterrupts();
        CARDCallback callback;
        s32 chan, result;

        if (pending_head == pending_tail) {
            OSRestoreInterrupts(enabled);
            return;
        }
        callback = pending[pending_head % MAX_PENDING].callback;
        chan = pending[pending_head % MAX_PENDING].chan;
        result = pending[pending_head % MAX_PENDING].result;
        pending_head++;
        OSRestoreInterrupts(enabled);
        ps3_diag.intr_site = "CARD callback";
        callback(chan, result);
    }
}

// The operation is done; its callback runs as an interrupt, after the caller enables interrupts again.
static s32 finish(s32 chan, s32 result, CARDCallback callback)
{
    BOOL enabled;

    last_result = result;
    if (callback == NULL) {
        return CARD_RESULT_READY;
    }
    enabled = OSDisableInterrupts();
    if (pending_tail - pending_head == MAX_PENDING) {
        OSRestoreInterrupts(enabled);
        OSReport("card: %d callbacks wait; one more is dropped\n", MAX_PENDING);
        return CARD_RESULT_READY;
    }
    pending[pending_tail % MAX_PENDING].callback = callback;
    pending[pending_tail % MAX_PENDING].chan = chan;
    pending[pending_tail % MAX_PENDING].result = result;
    pending_tail++;
    OSRestoreInterrupts(enabled);
    ps3_intr_raise(PS3_INTR_EXI);
    return CARD_RESULT_READY;
}

static u32 now_seconds(void)
{
    return (u32) OSTicksToSeconds(OSGetTime());
}

static void file_path(const CARDDir* d, char* out, size_t size)
{
    char name[CARD_FILENAME_MAX + 1];
    size_t i;

    memcpy(name, d->fileName, CARD_FILENAME_MAX);
    name[CARD_FILENAME_MAX] = '\0';
    for (i = 0; name[i] != '\0'; i++) {
        if (strchr("/\\:*?\"<>|", name[i]) != NULL || (u8) name[i] < 0x20 || (u8) name[i] > 0x7E) {
            name[i] = '_';
        }
    }
    snprintf(out, size, "%s/%.2s-%.4s-%s.gci", PS3_CARD_DIR, (const char*) d->company, (const char*) d->gameName,
             name);
}

// Writes [offset, offset + length) of the file's data, or only its directory entry with length 0.
static bool save(File* f, u32 offset, u32 length, bool whole)
{
    FILE* out = fopen(f->path, whole ? "wb" : "r+b");
    bool ok = out != NULL;

    if (ok && whole) {
        ok = fwrite(&f->dir, DIR_SIZE, 1, out) == 1 &&
             fwrite(f->data, (size_t) f->dir.length * SECTOR, 1, out) == 1;
    } else if (ok) {
        ok = fseek(out, 0, SEEK_SET) == 0 && fwrite(&f->dir, DIR_SIZE, 1, out) == 1;
        if (ok && length != 0) {
            ok = fseek(out, (long) (DIR_SIZE + offset), SEEK_SET) == 0 && fwrite(f->data + offset, length, 1, out) == 1;
        }
    }
    if (out != NULL && fclose(out) != 0) {
        ok = false;
    }
    if (!ok) {
        OSReport("card: writing %s failed\n", f->path);
    }
    return ok;
}

static void unload(void)
{
    int i;

    for (i = 0; i < CARD_MAX_FILE; i++) {
        free(files[i].data);
        memset(&files[i], 0, sizeof(files[i]));
    }
}

// Reads every GCI file in the folder whose size matches its directory entry.
static void load(void)
{
    DIR* d;
    struct dirent* e;
    int n = 0;

    unload();
    mkdir(PS3_CARD_DIR, 0777);
    d = opendir(PS3_CARD_DIR);
    if (d == NULL) {
        OSReport("card: no folder %s\n", PS3_CARD_DIR);
        return;
    }
    while ((e = readdir(d)) != NULL && n < CARD_MAX_FILE) {
        size_t len = strlen(e->d_name);
        File* f = &files[n];
        FILE* in;
        long size;

        if (len < 4 || strcmp(e->d_name + len - 4, ".gci") != 0) {
            continue;
        }
        snprintf(f->path, sizeof(f->path), "%s/%s", PS3_CARD_DIR, e->d_name);
        in = fopen(f->path, "rb");
        if (in == NULL) {
            continue;
        }
        if (fread(&f->dir, DIR_SIZE, 1, in) == 1 && fseek(in, 0, SEEK_END) == 0 &&
            (size = ftell(in)) == DIR_SIZE + (long) f->dir.length * SECTOR && f->dir.length != 0 &&
            (f->data = malloc((size_t) f->dir.length * SECTOR)) != NULL && fseek(in, DIR_SIZE, SEEK_SET) == 0 &&
            fread(f->data, (size_t) f->dir.length * SECTOR, 1, in) == 1)
        {
            f->used = true;
            OSReport("card: %s, %u blocks\n", e->d_name, f->dir.length);
            n++;
        } else {
            OSReport("card: %s is no GCI file of a whole number of blocks\n", e->d_name);
            free(f->data);
            memset(f, 0, sizeof(*f));
        }
        fclose(in);
    }
    closedir(d);
}

static u32 used_blocks(void)
{
    u32 blocks = 0;
    int i;

    for (i = 0; i < CARD_MAX_FILE; i++) {
        if (files[i].used) {
            blocks += files[i].dir.length;
        }
    }
    return blocks;
}

// As __CARDAccess: a file of this game, else NOPERM (NOFILE for a free entry).
static s32 access_file(s32 chan, s32 fileNo, bool public_ok)
{
    const DVDDiskID* id = DVDGetCurrentDiskID();
    File* f;

    if (chan != 0 || !mounted) {
        return CARD_RESULT_NOCARD;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    f = &files[fileNo];
    if (!f->used) {
        return CARD_RESULT_NOFILE;
    }
    if (memcmp(f->dir.gameName, id->gameName, 4) == 0 && memcmp(f->dir.company, id->company, 2) == 0) {
        return CARD_RESULT_READY;
    }
    return public_ok && (f->dir.permission & ATTR_PUBLIC) ? CARD_RESULT_READY : CARD_RESULT_NOPERM;
}

static s32 find(s32 chan, const char* name)
{
    s32 i;

    for (i = 0; i < CARD_MAX_FILE; i++) {
        if (access_file(chan, i, false) == CARD_RESULT_READY &&
            strncmp((const char*) files[i].dir.fileName, name, CARD_FILENAME_MAX) == 0)
        {
            return i;
        }
    }
    return -1;
}

static s32 open_info(s32 chan, s32 fileNo, CARDFileInfo* info)
{
    info->chan = chan;
    info->fileNo = fileNo;
    info->offset = 0;
    info->length = (s32) files[fileNo].dir.length * SECTOR;
    info->iBlock = files[fileNo].dir.startBlock;
    return CARD_RESULT_READY;
}

void CARDInit(void)
{
    ps3_intr_set_handler(PS3_INTR_EXI, dispatch);
}

s32 CARDGetResultCode(s32 chan)
{
    return chan == 0 ? last_result : CARD_RESULT_NOCARD;
}

BOOL CARDProbe(long chan)
{
    ps3_diag.card_calls++;
    return chan == 0;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
    ps3_diag.card_calls++;
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    if (memSize != NULL) {
        *memSize = MEM_MBITS;
    }
    if (sectorSize != NULL) {
        *sectorSize = SECTOR;
    }
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCallback, CARDCallback attachCallback)
{
    ps3_diag.card_calls++;
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    if (!mounted) {
        load();
        mounted = true;
    }
    return finish(chan, CARD_RESULT_READY, attachCallback);
}

s32 CARDUnmount(s32 chan)
{
    ps3_diag.card_calls++;
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    mounted = false;
    unload();
    return CARD_RESULT_READY;
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback)
{
    ps3_diag.card_calls++;
    if (chan != 0 || !mounted) {
        return CARD_RESULT_NOCARD;
    }
    return finish(chan, CARD_RESULT_READY, callback);
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback)
{
    int i;

    ps3_diag.card_calls++;
    if (chan != 0 || !mounted) {
        return CARD_RESULT_NOCARD;
    }
    for (i = 0; i < CARD_MAX_FILE; i++) {
        if (files[i].used) {
            remove(files[i].path);
        }
    }
    unload();
    return finish(chan, CARD_RESULT_READY, callback);
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
    s32 n = 0;
    int i;

    ps3_diag.card_calls++;
    if (chan != 0 || !mounted) {
        return CARD_RESULT_NOCARD;
    }
    for (i = 0; i < CARD_MAX_FILE; i++) {
        n += !files[i].used;
    }
    if (byteNotUsed != NULL) {
        *byteNotUsed = (s32) (USER_BLOCKS - used_blocks()) * SECTOR;
    }
    if (filesNotUsed != NULL) {
        *filesNotUsed = n;
    }
    return CARD_RESULT_READY;
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo, CARDCallback callback)
{
    const DVDDiskID* id = DVDGetCurrentDiskID();
    File* f = NULL;
    s32 i;

    ps3_diag.card_calls++;
    if (chan != 0 || !mounted) {
        return CARD_RESULT_NOCARD;
    }
    if (strlen(fileName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    if (size == 0 || size % SECTOR != 0) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (find(chan, fileName) >= 0) {
        return CARD_RESULT_EXIST;
    }
    for (i = 0; i < CARD_MAX_FILE && files[i].used; i++) {
    }
    if (i == CARD_MAX_FILE) {
        return CARD_RESULT_NOENT;
    }
    if (used_blocks() + size / SECTOR > USER_BLOCKS) {
        return CARD_RESULT_INSSPACE;
    }
    f = &files[i];
    memset(f, 0, sizeof(*f));
    f->data = calloc(1, size);
    if (f->data == NULL) {
        return CARD_RESULT_INSSPACE;
    }
    memset(&f->dir, 0xFF, sizeof(f->dir));
    memcpy(f->dir.gameName, id->gameName, 4);
    memcpy(f->dir.company, id->company, 2);
    f->dir.bannerFormat = 0;
    memset(f->dir.fileName, 0, CARD_FILENAME_MAX);
    strncpy((char*) f->dir.fileName, fileName, CARD_FILENAME_MAX);
    f->dir.time = now_seconds();
    f->dir.iconAddr = 0xFFFFFFFF;
    f->dir.iconFormat = 0;
    f->dir.iconSpeed = CARD_STAT_SPEED_FAST;
    f->dir.permission = ATTR_PUBLIC;
    f->dir.copyTimes = 0;
    // A first block for the file number only: the files have no block map here.
    f->dir.startBlock = (u16) (5 + i);
    f->dir.length = (u16) (size / SECTOR);
    f->dir.commentAddr = 0xFFFFFFFF;
    file_path(&f->dir, f->path, sizeof(f->path));
    f->used = true;
    if (fileInfo != NULL) {
        open_info(chan, i, fileInfo);
    }
    return finish(chan, save(f, 0, 0, true) ? CARD_RESULT_READY : CARD_RESULT_IOERROR, callback);
}

s32 CARDDeleteAsync(s32 chan, char* fileName, CARDCallback callback)
{
    s32 i;

    ps3_diag.card_calls++;
    if (chan != 0 || !mounted) {
        return CARD_RESULT_NOCARD;
    }
    if ((i = find(chan, fileName)) < 0) {
        return CARD_RESULT_NOFILE;
    }
    remove(files[i].path);
    free(files[i].data);
    memset(&files[i], 0, sizeof(files[i]));
    return finish(chan, CARD_RESULT_READY, callback);
}

s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName, CARDCallback callback)
{
    char path[256];
    File* f;
    s32 i;

    ps3_diag.card_calls++;
    if (chan != 0 || !mounted) {
        return CARD_RESULT_NOCARD;
    }
    if (strlen(newName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    if ((i = find(chan, oldName)) < 0) {
        return CARD_RESULT_NOFILE;
    }
    if (find(chan, newName) >= 0) {
        return CARD_RESULT_EXIST;
    }
    f = &files[i];
    memset(f->dir.fileName, 0, CARD_FILENAME_MAX);
    strncpy((char*) f->dir.fileName, newName, CARD_FILENAME_MAX);
    f->dir.time = now_seconds();
    file_path(&f->dir, path, sizeof(path));
    rename(f->path, path);
    snprintf(f->path, sizeof(f->path), "%s", path);
    return finish(chan, save(f, 0, 0, false) ? CARD_RESULT_READY : CARD_RESULT_IOERROR, callback);
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fileInfo)
{
    s32 i;

    ps3_diag.card_calls++;
    if (chan != 0 || !mounted) {
        return CARD_RESULT_NOCARD;
    }
    if ((i = find(chan, fileName)) < 0) {
        return CARD_RESULT_NOFILE;
    }
    return open_info(chan, i, fileInfo);
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* fileInfo)
{
    s32 result;

    ps3_diag.card_calls++;
    result = access_file(chan, fileNo, true);
    if (result < 0) {
        return result;
    }
    return open_info(chan, fileNo, fileInfo);
}

s32 CARDClose(CARDFileInfo* fileInfo)
{
    ps3_diag.card_calls++;
    if (fileInfo->chan != 0 || !mounted) {
        return CARD_RESULT_NOCARD;
    }
    fileInfo->chan = -1;
    return CARD_RESULT_READY;
}

static s32 check_range(const CARDFileInfo* fileInfo, s32 length, s32 offset)
{
    s32 result = access_file(fileInfo->chan, fileInfo->fileNo, true);

    if (result < 0) {
        return result;
    }
    if (length < 0 || offset < 0 || (u32) offset + (u32) length > (u32) files[fileInfo->fileNo].dir.length * SECTOR) {
        return CARD_RESULT_LIMIT;
    }
    return CARD_RESULT_READY;
}

s32 CARDReadAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, CARDCallback callback)
{
    s32 result;

    ps3_diag.card_calls++;
    if ((result = check_range(fileInfo, length, offset)) < 0) {
        return result;
    }
    memcpy(buf, files[fileInfo->fileNo].data + offset, (size_t) length);
    DCStoreRange(buf, (u32) length);
    xferred += length;
    return finish(fileInfo->chan, CARD_RESULT_READY, callback);
}

s32 CARDWriteAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, CARDCallback callback)
{
    File* f;
    s32 result;

    ps3_diag.card_calls++;
    if ((result = check_range(fileInfo, length, offset)) < 0) {
        return result;
    }
    if (access_file(fileInfo->chan, fileInfo->fileNo, false) < 0) {
        return CARD_RESULT_NOPERM;
    }
    f = &files[fileInfo->fileNo];
    memcpy(f->data + offset, buf, (size_t) length);
    f->dir.time = now_seconds();
    xferred += length;
    return finish(fileInfo->chan, save(f, (u32) offset, (u32) length, false) ? CARD_RESULT_READY : CARD_RESULT_IOERROR,
                  callback);
}

// As CARDStat.c's UpdateIconOffsets: the banner, the icons and their palettes lie at iconAddr in that order.
static void icon_offsets(const CARDDir* d, CARDStat* stat)
{
    u32 offset = d->iconAddr;
    bool tlut = false;
    int i;

    if (offset == 0xFFFFFFFF) {
        stat->bannerFormat = 0;
        stat->iconFormat = 0;
        stat->iconSpeed = 0;
        offset = 0;
    }
    switch (stat->bannerFormat & CARD_STAT_BANNER_MASK) {
    case CARD_STAT_BANNER_C8:
        stat->offsetBanner = offset;
        offset += BANNER_W * BANNER_H;
        stat->offsetBannerTlut = offset;
        offset += 2 * 256;
        break;
    case CARD_STAT_BANNER_RGB5A3:
        stat->offsetBanner = offset;
        offset += 2 * BANNER_W * BANNER_H;
        stat->offsetBannerTlut = 0xFFFFFFFF;
        break;
    default:
        stat->offsetBanner = 0xFFFFFFFF;
        stat->offsetBannerTlut = 0xFFFFFFFF;
        break;
    }
    for (i = 0; i < CARD_ICON_MAX; i++) {
        switch ((stat->iconFormat >> (2 * i)) & CARD_STAT_ICON_MASK) {
        case CARD_STAT_ICON_C8:
            stat->offsetIcon[i] = offset;
            offset += ICON_W * ICON_H;
            tlut = true;
            break;
        case CARD_STAT_ICON_RGB5A3:
            stat->offsetIcon[i] = offset;
            offset += 2 * ICON_W * ICON_H;
            break;
        default:
            stat->offsetIcon[i] = 0xFFFFFFFF;
            break;
        }
    }
    stat->offsetIconTlut = tlut ? offset : 0xFFFFFFFF;
    offset += tlut ? 2 * 256 : 0;
    stat->offsetData = offset;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat)
{
    const CARDDir* d;
    s32 result;

    ps3_diag.card_calls++;
    if ((result = access_file(chan, fileNo, true)) < 0) {
        return result;
    }
    d = &files[fileNo].dir;
    memcpy(stat->gameName, d->gameName, 4);
    memcpy(stat->company, d->company, 2);
    stat->length = (u32) d->length * SECTOR;
    memcpy(stat->fileName, d->fileName, CARD_FILENAME_MAX);
    stat->time = d->time;
    stat->bannerFormat = d->bannerFormat;
    stat->iconAddr = d->iconAddr;
    stat->iconFormat = d->iconFormat;
    stat->iconSpeed = d->iconSpeed;
    stat->commentAddr = d->commentAddr;
    icon_offsets(d, stat);
    return CARD_RESULT_READY;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* stat, CARDCallback callback)
{
    CARDDir* d;
    s32 result;

    ps3_diag.card_calls++;
    if ((result = access_file(chan, fileNo, false)) < 0) {
        return result;
    }
    d = &files[fileNo].dir;
    d->bannerFormat = stat->bannerFormat;
    d->iconAddr = stat->iconAddr;
    d->iconFormat = stat->iconFormat;
    d->iconSpeed = stat->iconSpeed;
    d->commentAddr = stat->commentAddr;
    icon_offsets(d, stat);
    if (d->iconAddr == 0xFFFFFFFF) {
        d->iconSpeed = (u16) ((d->iconSpeed & ~CARD_STAT_SPEED_MASK) | CARD_STAT_SPEED_FAST);
    }
    d->time = now_seconds();
    return finish(chan, save(&files[fileNo], 0, 0, false) ? CARD_RESULT_READY : CARD_RESULT_IOERROR, callback);
}

long CARDGetXferredBytes(long chan)
{
    ps3_diag.card_calls++;
    return chan == 0 ? xferred : 0;
}
