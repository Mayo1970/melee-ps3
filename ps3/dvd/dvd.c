// The SDK's DVD functions over a disc image or an extracted disc on the HDD or a USB drive.
// A read happens in the call; its callback waits for ps3_dvd_dispatch, as it waited for the DVD interrupt.
#include "dvd.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include "../os/diag.h"
#include "../os/intr.h"
#include "../os/sync.h"

#ifndef PS3_APP_DIR
// Placeholder until the package sets the title ID.
#define PS3_APP_DIR "/dev_hdd0/game/MELE00001/USRDIR"
#endif

#define DISC_MAGIC 0xC2339F3D
#define HEADER_SIZE 0x440
#define FST_ENTRY_SIZE 12
#define CISO_HEADER_SIZE 0x8000
#define CISO_MAP_SIZE (CISO_HEADER_SIZE - 8)
#define CISO_UNUSED 0xFFFFFFFF
#define MAX_PENDING 32
#define MAX_SCAN_DEPTH 3
#define MAX_FST_DEPTH 32
#define PATH_SIZE 512

_Static_assert(sizeof(DVDDiskID) == 0x20, "DVDDiskID is the first 32 bytes of the disc header");

enum Source {
    SRC_NONE,
    SRC_IMAGE,
    SRC_CISO,
    SRC_FOLDER,
};

static struct {
    enum Source source;
    FILE* image;
    u32 ciso_block_size;
    u32* ciso_map;
    char root[PATH_SIZE];
    u8* fst;
    u32 fst_size;
    u32 entries;
    u32* parent;
    // Folder mode keeps the last file open: devcom reads a file in 512 KB parts.
    FILE* open_file;
    u32 open_entry;
    DVDDiskID id;
} disc;

// Serializes the file position of image and open_file between threads.
static PsMutex io_lock = PS_MUTEX_INIT;

static struct {
    DVDFileInfo* file;
    DVDCallback callback;
    s32 result;
} pending[MAX_PENDING];
static u32 pending_head, pending_tail;
static void (*notify)(void);

static u32 be32(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

static u32 le32(const u8* p)
{
    return (u32) p[3] << 24 | (u32) p[2] << 16 | (u32) p[1] << 8 | p[0];
}

static const u8* entry_at(u32 i)
{
    return disc.fst + i * FST_ENTRY_SIZE;
}

static bool entry_is_dir(u32 i)
{
    return entry_at(i)[0] != 0;
}

static const char* entry_name(u32 i)
{
    return (const char*) disc.fst + disc.entries * FST_ENTRY_SIZE +
           (be32(entry_at(i)) & 0xFFFFFF);
}

// A file's disc offset; a directory's parent.
static u32 entry_offset(u32 i)
{
    return be32(entry_at(i) + 4);
}

// A file's length; for a directory, the entry after its last child.
static u32 entry_size(u32 i)
{
    return be32(entry_at(i) + 8);
}

// Reads disc bytes from a plain or CISO image. Bytes past the image end read as 0.
static bool image_read(u64 offset, void* dst, u32 length)
{
    u8* out = dst;

    while (length != 0) {
        u64 at = offset;
        u32 n = length;
        bool stored = true;

        if (disc.source == SRC_CISO) {
            u64 block = offset / disc.ciso_block_size;
            u32 within = (u32) (offset % disc.ciso_block_size);
            if (n > disc.ciso_block_size - within) {
                n = disc.ciso_block_size - within;
            }
            stored = block < CISO_MAP_SIZE && disc.ciso_map[block] != CISO_UNUSED;
            if (stored) {
                at = CISO_HEADER_SIZE + (u64) disc.ciso_map[block] * disc.ciso_block_size +
                     within;
            }
        }
        if (stored) {
            size_t got;
            if (fseek(disc.image, (long) at, SEEK_SET) != 0) {
                return false;
            }
            got = fread(out, 1, n, disc.image);
            if (ferror(disc.image)) {
                return false;
            }
            memset(out + got, 0, n - got);
        } else {
            memset(out, 0, n);
        }
        out += n;
        offset += n;
        length -= n;
    }
    return true;
}

// The file of an entry, under root/files, from its parent chain.
static bool entry_path(u32 entry, char* out, size_t size)
{
    u32 chain[MAX_FST_DEPTH];
    int count = 0;
    size_t len;

    for (; entry != 0; entry = disc.parent[entry]) {
        if (count == MAX_FST_DEPTH) {
            return false;
        }
        chain[count++] = entry;
    }
    len = (size_t) snprintf(out, size, "%s/files", disc.root);
    while (count > 0 && len < size) {
        len += (size_t) snprintf(out + len, size - len, "/%s", entry_name(chain[--count]));
    }
    return len < size;
}

// Bytes past the file end read as 0, as the GameCube reads 32-byte rounded sizes.
static bool folder_read(u32 entry, u32 offset, void* dst, u32 length)
{
    size_t got;

    if (disc.open_entry != entry) {
        char path[PATH_SIZE] = "";
        if (disc.open_file != NULL) {
            fclose(disc.open_file);
        }
        disc.open_entry = 0;
        disc.open_file = entry_path(entry, path, sizeof(path)) ? fopen(path, "rb") : NULL;
        if (disc.open_file == NULL) {
            OSReport("dvd: cannot open %s\n", path);
            return false;
        }
        disc.open_entry = entry;
    }
    if (fseek(disc.open_file, (long) offset, SEEK_SET) != 0) {
        return false;
    }
    got = fread(dst, 1, length, disc.open_file);
    if (ferror(disc.open_file)) {
        return false;
    }
    memset((u8*) dst + got, 0, length - got);
    return true;
}

static u8* read_whole(const char* path, u32* size)
{
    FILE* f = fopen(path, "rb");
    long len;
    u8* data = NULL;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        // One more byte, so the last FST name always ends in a NUL.
        data = calloc(1, (size_t) len + 1);
        if (data != NULL && fread(data, 1, (size_t) len, f) != (size_t) len) {
            free(data);
            data = NULL;
        }
        *size = (u32) len;
    }
    fclose(f);
    return data;
}

static void unmount(void)
{
    if (disc.open_file != NULL) {
        fclose(disc.open_file);
    }
    if (disc.image != NULL) {
        fclose(disc.image);
    }
    free(disc.ciso_map);
    free(disc.fst);
    free(disc.parent);
    memset(&disc, 0, sizeof(disc));
}

static bool open_image(const char* path, u8* header)
{
    u8 magic[8];
    u32 fst_offset;

    disc.image = fopen(path, "rb");
    if (disc.image == NULL || fread(magic, 1, sizeof(magic), disc.image) != sizeof(magic)) {
        return false;
    }
    disc.source = SRC_IMAGE;
    if (memcmp(magic, "CISO", 4) == 0) {
        u8* map = malloc(CISO_MAP_SIZE);
        u32 stored = 0;
        u32 i;

        disc.ciso_block_size = le32(magic + 4);
        disc.ciso_map = malloc(CISO_MAP_SIZE * sizeof(u32));
        if (map == NULL || disc.ciso_map == NULL || disc.ciso_block_size == 0 ||
            fread(map, 1, CISO_MAP_SIZE, disc.image) != CISO_MAP_SIZE)
        {
            free(map);
            return false;
        }
        for (i = 0; i < CISO_MAP_SIZE; i++) {
            disc.ciso_map[i] = map[i] == 1 ? stored++ : CISO_UNUSED;
        }
        free(map);
        disc.source = SRC_CISO;
    }
    if (!image_read(0, header, HEADER_SIZE)) {
        return false;
    }
    fst_offset = be32(header + 0x424);
    disc.fst_size = be32(header + 0x428);
    if (disc.fst_size < FST_ENTRY_SIZE || disc.fst_size > 0x1000000) {
        return false;
    }
    disc.fst = calloc(1, disc.fst_size + 1);
    return disc.fst != NULL && image_read(fst_offset, disc.fst, disc.fst_size);
}

static bool open_folder(const char* path, u8* header)
{
    char file[PATH_SIZE];
    u32 size = 0;
    u8* boot;

    disc.source = SRC_FOLDER;
    snprintf(disc.root, sizeof(disc.root), "%s", path);
    snprintf(file, sizeof(file), "%s/sys/boot.bin", path);
    boot = read_whole(file, &size);
    if (boot == NULL || size < HEADER_SIZE) {
        free(boot);
        return false;
    }
    memcpy(header, boot, HEADER_SIZE);
    free(boot);
    snprintf(file, sizeof(file), "%s/sys/fst.bin", path);
    disc.fst = read_whole(file, &disc.fst_size);
    return disc.fst != NULL;
}

// Checks the FST and finds each entry's parent directory.
static bool load_fst(void)
{
    u32 stack[MAX_FST_DEPTH];
    u32 ends[MAX_FST_DEPTH];
    int depth = 0;
    u32 names_size;
    u32 i;

    disc.entries = be32(disc.fst + 8);
    if (disc.fst_size < FST_ENTRY_SIZE || disc.entries == 0 ||
        disc.entries > disc.fst_size / FST_ENTRY_SIZE)
    {
        return false;
    }
    names_size = disc.fst_size - disc.entries * FST_ENTRY_SIZE;
    disc.parent = calloc(disc.entries, sizeof(u32));
    if (disc.parent == NULL) {
        return false;
    }
    stack[0] = 0;
    ends[0] = disc.entries;
    for (i = 1; i < disc.entries; i++) {
        if ((be32(entry_at(i)) & 0xFFFFFF) >= names_size) {
            return false;
        }
        while (depth > 0 && i >= ends[depth]) {
            depth--;
        }
        disc.parent[i] = stack[depth];
        if (entry_is_dir(i)) {
            if (entry_size(i) <= i || entry_size(i) > ends[depth] || depth + 1 == MAX_FST_DEPTH) {
                return false;
            }
            depth++;
            stack[depth] = i;
            ends[depth] = entry_size(i);
        }
    }
    return true;
}

bool ps3_dvd_mount(const char* path)
{
    u8 header[HEADER_SIZE];
    struct stat st;
    bool ok;

    ps_mutex_lock(&io_lock);
    unmount();
    if (stat(path, &st) != 0) {
        ps_mutex_unlock(&io_lock);
        OSReport("dvd: %s not found\n", path);
        return false;
    }
    ok = S_ISDIR(st.st_mode) ? open_folder(path, header) : open_image(path, header);
    ok = ok && be32(header + 0x1C) == DISC_MAGIC && load_fst();
    if (!ok) {
        unmount();
        ps_mutex_unlock(&io_lock);
        OSReport("dvd: %s is not a GameCube disc\n", path);
        return false;
    }
    memcpy(&disc.id, header, sizeof(disc.id));
    ps_mutex_unlock(&io_lock);

    OSReport("dvd: %s: %.4s%.2s revision %d, %u entries\n", path, disc.id.gameName,
             disc.id.company, disc.id.gameVersion, disc.entries);
    if (memcmp(disc.id.gameName, "GALE", 4) != 0 || memcmp(disc.id.company, "01", 2) != 0 ||
        disc.id.gameVersion != 2)
    {
        OSReport("dvd: the decomp is Melee 1.02, GALE01 revision 2\n");
    }
    return true;
}

// The disc ID of an image or extracted folder, without mounting it.
static bool probe(const char* path, bool folder, DVDDiskID* id)
{
    char file[PATH_SIZE];
    u8 header[CISO_HEADER_SIZE];
    long at = 0;
    FILE* f;
    bool ok;

    if (folder) {
        snprintf(file, sizeof(file), "%s/sys/boot.bin", path);
        path = file;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    ok = fread(header, 1, 0x20, f) == 0x20;
    // A CISO stores the disc from 0x8000 on, if its first block is present.
    if (ok && !folder && memcmp(header, "CISO", 4) == 0) {
        ok = fread(header + 0x20, 1, CISO_HEADER_SIZE - 0x20, f) == CISO_HEADER_SIZE - 0x20 &&
             header[8] == 1;
        at = CISO_HEADER_SIZE;
        ok = ok && fseek(f, at, SEEK_SET) == 0 && fread(header, 1, 0x20, f) == 0x20;
    }
    fclose(f);
    if (!ok || be32(header + 0x1C) != DISC_MAGIC) {
        return false;
    }
    memcpy(id, header, sizeof(*id));
    return true;
}

static bool has_disc_extension(const char* name)
{
    static const char* const exts[] = { ".iso", ".gcm", ".ciso" };
    size_t len = strlen(name);
    size_t i;

    for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t n = strlen(exts[i]);
        if (len > n && strcasecmp(name + len - n, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

// want: a game ID from disc.txt, or empty for GALE01 revision 2.
static bool is_wanted(const DVDDiskID* id, const char* want)
{
    if (want[0] != '\0') {
        return strncasecmp(id->gameName, want, 4) == 0 &&
               strncasecmp(id->company, want + 4, 2) == 0;
    }
    return memcmp(id->gameName, "GALE", 4) == 0 && memcmp(id->company, "01", 2) == 0 &&
           id->gameVersion == 2;
}

static bool scan(const char* dir, int depth, const char* want, char* out)
{
    DIR* d = opendir(dir);
    struct dirent* e;
    bool found = false;

    if (d == NULL) {
        return false;
    }
    while (!found && (e = readdir(d)) != NULL) {
        char path[PATH_SIZE];
        struct stat st;
        DVDDiskID id;
        bool folder;

        if (e->d_name[0] == '.' ||
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int) sizeof(path) ||
            stat(path, &st) != 0)
        {
            continue;
        }
        folder = S_ISDIR(st.st_mode);
        if ((folder || has_disc_extension(e->d_name)) && probe(path, folder, &id)) {
            OSReport("dvd: found %.4s%.2s revision %d at %s\n", id.gameName, id.company,
                     id.gameVersion, path);
            if (is_wanted(&id, want)) {
                snprintf(out, PATH_SIZE, "%s", path);
                found = true;
            }
        } else if (folder && depth < MAX_SCAN_DEPTH) {
            found = scan(path, depth + 1, want, out);
        }
    }
    closedir(d);
    return found;
}

bool ps3_dvd_mount_default(void)
{
    char want[PATH_SIZE] = "";
    char path[PATH_SIZE];
    FILE* f = fopen(PS3_APP_DIR "/disc.txt", "r");
    int i;

    if (f != NULL) {
        if (fgets(want, sizeof(want), f) != NULL) {
            want[strcspn(want, "\r\n")] = '\0';
        }
        fclose(f);
    }
    if (strchr(want, '/') != NULL) {
        return ps3_dvd_mount(want);
    }
    if (want[0] != '\0' && strlen(want) != 6) {
        OSReport("dvd: disc.txt holds \"%s\", which is neither a game ID nor a path\n", want);
        return false;
    }
    if (scan(PS3_APP_DIR "/games", 0, want, path)) {
        return ps3_dvd_mount(path);
    }
    for (i = 0; i < 8; i++) {
        char root[32];
        snprintf(root, sizeof(root), "/dev_usb%03d/games", i);
        if (scan(root, 0, want, path)) {
            return ps3_dvd_mount(path);
        }
    }
    OSReport("dvd: no %s disc in %s/games or /dev_usbNNN/games\n",
             want[0] != '\0' ? want : "GALE01 revision 2", PS3_APP_DIR);
    return false;
}

void ps3_dvd_set_notify(void (*fn)(void))
{
    notify = fn;
}

void ps3_dvd_dispatch(void)
{
    for (;;) {
        BOOL enabled = OSDisableInterrupts();
        DVDFileInfo* file;
        DVDCallback callback;
        s32 result;

        if (pending_head == pending_tail) {
            OSRestoreInterrupts(enabled);
            return;
        }
        file = pending[pending_head % MAX_PENDING].file;
        callback = pending[pending_head % MAX_PENDING].callback;
        result = pending[pending_head % MAX_PENDING].result;
        pending_head++;
        file->cb.state = DVD_STATE_END;
        OSRestoreInterrupts(enabled);
        ps3_diag.dvd_callbacks++;
        ps3_diag.intr_site = "DVD callback";
        callback(result, file);
    }
}

static BOOL post(DVDFileInfo* file, DVDCallback callback, s32 result)
{
    BOOL enabled = OSDisableInterrupts();

    if (pending_tail - pending_head == MAX_PENDING) {
        OSRestoreInterrupts(enabled);
        OSReport("dvd: %d reads wait for dispatch; one more is refused\n", MAX_PENDING);
        return FALSE;
    }
    pending[pending_tail % MAX_PENDING].file = file;
    pending[pending_tail % MAX_PENDING].callback = callback;
    pending[pending_tail % MAX_PENDING].result = result;
    pending_tail++;
    file->cb.state = DVD_STATE_BUSY;
    OSRestoreInterrupts(enabled);
    if (notify != NULL) {
        notify();
    }
    return TRUE;
}

static void raise_dvd(void)
{
    ps3_intr_raise(PS3_INTR_DVD);
}

void DVDInit(void)
{
    ps3_intr_set_handler(PS3_INTR_DVD, ps3_dvd_dispatch);
    ps3_dvd_set_notify(raise_dvd);
    if (disc.source == SRC_NONE) {
        ps3_dvd_mount_default();
    }
}

static s32 path_to_entry(const char* path)
{
    u32 cur = 0;

    if (disc.source == SRC_NONE) {
        return -1;
    }
    // Melee never calls DVDChangeDir, so every path starts at the root.
    while (*path != '\0') {
        const char* end;
        size_t len;
        u32 i;

        while (*path == '/') {
            path++;
        }
        if (*path == '\0') {
            break;
        }
        end = strchr(path, '/');
        len = end != NULL ? (size_t) (end - path) : strlen(path);
        if (len == 2 && path[0] == '.' && path[1] == '.') {
            cur = disc.parent[cur];
        } else if (len != 1 || path[0] != '.') {
            if (!entry_is_dir(cur)) {
                return -1;
            }
            // The SDK compares names without case.
            for (i = cur + 1; i < entry_size(cur); i = entry_is_dir(i) ? entry_size(i) : i + 1) {
                const char* name = entry_name(i);
                if (strncasecmp(name, path, len) == 0 && name[len] == '\0') {
                    break;
                }
            }
            if (i >= entry_size(cur)) {
                return -1;
            }
            cur = i;
        }
        path += len;
    }
    return (s32) cur;
}

// Each path in the log, as a trail of what the game loads (step 3 diagnostics).
s32 DVDConvertPathToEntrynum(const char* path)
{
    s32 entry = path_to_entry(path);

    OSReport("dvd: %s -> %d\n", path, entry);
    return entry;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo)
{
    if (disc.source == SRC_NONE || entrynum <= 0 || (u32) entrynum >= disc.entries ||
        entry_is_dir((u32) entrynum))
    {
        return FALSE;
    }
    // A folder has no disc offsets, so startAddr holds the entry there.
    fileInfo->startAddr =
        disc.source == SRC_FOLDER ? (u32) entrynum : entry_offset((u32) entrynum);
    fileInfo->length = entry_size((u32) entrynum);
    fileInfo->callback = NULL;
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDClose(DVDFileInfo* fileInfo)
{
    return TRUE;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                      DVDCallback callback, s32 prio)
{
    bool ok = disc.source != SRC_NONE && length >= 0 && offset >= 0 &&
              (u32) offset <= fileInfo->length;

    ps3_diag.dvd_reads++;
    if (ok) {
        ps_mutex_lock(&io_lock);
        ok = disc.source == SRC_FOLDER
                 ? folder_read(fileInfo->startAddr, (u32) offset, addr, (u32) length)
                 : image_read((u64) fileInfo->startAddr + (u32) offset, addr, (u32) length);
        ps_mutex_unlock(&io_lock);
    }
    if (!ok) {
        OSReport("dvd: reading %d bytes at %d failed\n", length, offset);
    }
    fileInfo->callback = callback;
    if (callback == NULL) {
        return ok;
    }
    return post(fileInfo, callback, ok ? length : DVD_RESULT_FATAL_ERROR);
}

DVDDiskID* DVDGetCurrentDiskID(void)
{
    return &disc.id;
}

// Reads never wait for a drive, and no cover opens.
long DVDGetDriveStatus(void)
{
    return DVD_STATE_END;
}

BOOL DVDCheckDisk(void)
{
    return disc.source != SRC_NONE;
}
