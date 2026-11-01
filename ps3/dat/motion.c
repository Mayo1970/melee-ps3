// Fighter motions parsed once (ps3_dat.h). The game copies each motion's archive into one of its fighter's two
// buffers and parses it there (ftdata.c), about 20 times a second in a 4-player match, mostly the same motions.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "melee_dat.h"
#include <dat/reader-internal.h>
#include <dolphin/os.h>
#include <melee/lb/lbanim.h>
#include <sysdolphin/baselib/archive.h>

#include <ps3_dat.h>
#include <ps3_rollback.h>

#include "../os/mem.h"
#include "../os/prof.h"
#include "lookup.h"
#include "pool.h"

// The pool bytes the entries may take; past that, motions are parsed in place as before, until the next scene.
#define CACHE_BYTES (8u << 20)
// What a new scene keeps of them, the least recently used going first, so its fighters' motions have room.
#define SCENE_BYTES (CACHE_BYTES / 2)
#define NBUCKETS 1024

// An entry owns its copy of the archive, which its natives point into, and the whole native archive. The game
// only reads FigaTree and FigaTrack (ftanim.c, lbanim.c) and the FObj data (fobj.c), so fighters share them.
typedef struct Motion {
    struct Motion* next;
    struct Motion *newer, *older;
    uint64_t hash;
    u32 size, bytes;
    u8* data;
    DatArchive* native;
    FigaTree* tree;
    char name[];
} Motion;

s32 __real_HSD_ArchiveParse(HSD_Archive* archive, u8* src, size_t size);

static Motion* buckets[NBUCKETS];
static Motion* newest;
static Motion* oldest;
static u32 nentries, nbytes;
static bool full_logged;
static int32_t figatree = DAT_NONE;

// Two multiply chains, so the PPE overlaps their latencies.
static uint64_t hash_words(const u8* p, u32 size)
{
    uint64_t a = 0xCBF29CE484222325ull, b = size;
    u32 i;

    for (i = 0; i + 16 <= size; i += 16) {
        uint64_t w0, w1;

        memcpy(&w0, p + i, 8);
        memcpy(&w1, p + i + 8, 8);
        a = (a ^ w0) * 0x100000001B3ull;
        b = (b ^ w1) * 0x100000001B3ull;
    }
    for (; i < size; i++) {
        a = (a ^ p[i]) * 0x100000001B3ull;
    }
    return a ^ (b >> 7) ^ (b << 29);
}

static Motion** bucket(uint64_t hash)
{
    return &buckets[(hash ^ hash >> 32) & (NBUCKETS - 1)];
}

static void unlink_use(Motion* m)
{
    *(m->newer != NULL ? &m->newer->older : &newest) = m->older;
    *(m->older != NULL ? &m->older->newer : &oldest) = m->newer;
}

static void link_newest(Motion* m)
{
    m->newer = NULL;
    m->older = newest;
    *(newest != NULL ? &newest->newer : &oldest) = m;
    newest = m;
}

static Motion* find(uint64_t hash, const u8* src, u32 size, const char* name)
{
    Motion* m;

    for (m = *bucket(hash); m != NULL; m = m->next) {
        if (m->hash == hash && m->size == size && strcmp(m->name, name) == 0 && memcmp(m->data, src, size) == 0) {
            return m;
        }
    }
    return NULL;
}

static FigaTree* lookup(DatArchive* native, const char* name)
{
    if (figatree == DAT_NONE) {
        figatree = melee_dat_type("FigaTree");
    }
    return figatree != DAT_NONE ? dat_public(native, name, figatree) : NULL;
}

// The HSD_Archive's tables: the game keeps nothing of a motion's archive but the FigaTree.
static void free_tables(HSD_Archive* archive)
{
    free(archive->reloc_info);
    free(archive->public_info);
    free(archive->extern_info);
}

// The real parse and the reader, not the wrapped calls: those would track the copy and log.
static Motion* add(uint64_t hash, const u8* src, u32 size, const char* name)
{
    size_t len = strlen(name) + 1, before, after, most;
    HSD_Archive archive;
    FigaTree* tree;
    Motion* m;
    u8* data;

    ps3_dat_pool_usage(&before, &most);
    data = malloc(size);
    if (data == NULL) {
        return NULL;
    }
    memcpy(data, src, size);
    if (__real_HSD_ArchiveParse(&archive, data, size) != 0) {
        free(data);
        return NULL;
    }
    free_tables(&archive);
    tree = lookup(archive.native_data, name);
    m = tree != NULL ? malloc(sizeof(*m) + len) : NULL;
    if (m == NULL) {
        dat_close(archive.native_data);
        free(data);
        return NULL;
    }
    ps3_dat_pool_usage(&after, &most);
    m->hash = hash;
    m->size = size;
    m->bytes = after > before ? (u32) (after - before) : 0;
    m->data = data;
    m->native = archive.native_data;
    m->tree = tree;
    memcpy(m->name, name, len);
    m->next = *bucket(hash);
    *bucket(hash) = m;
    link_newest(m);
    nentries++;
    nbytes += m->bytes;
    if (DAT_TRACE) {
        OSReport("dat: motion %s: %u bytes cached in %u; %u entries, %u KB\n", name, size, m->bytes, nentries,
                 nbytes >> 10);
    }
    return m;
}

static void drop(Motion* m)
{
    Motion** at = bucket(m->hash);

    while (*at != m) {
        at = &(*at)->next;
    }
    *at = m->next;
    unlink_use(m);
    nentries--;
    nbytes -= m->bytes;
    dat_close(m->native);
    free(m->data);
    free(m);
}

#ifdef DAT_MOTION_CHECK
static u32 checks, check_hits, differences;

// 0 when the game reads the same from both trees, else the first field that differs.
static int compare(const FigaTree* a, const u8* a_file, const FigaTree* b, const u8* b_file)
{
    int i, ntracks = 0;

    if (a->type != b->type || a->flags != b->flags || memcmp(&a->frames, &b->frames, sizeof(a->frames)) != 0) {
        return 1;
    }
    if ((a->nodes == NULL) != (b->nodes == NULL)) {
        return 2;
    }
    for (i = 0; a->nodes != NULL && a->nodes[i] != -1; i++) {
        if (a->nodes[i] != b->nodes[i]) {
            return 3;
        }
        ntracks += a->nodes[i];
    }
    if (a->nodes != NULL && b->nodes[i] != -1) {
        return 3;
    }
    if (ntracks > 0 && (a->tracks == NULL || b->tracks == NULL)) {
        return 4;
    }
    for (i = 0; i < ntracks; i++) {
        const FigaTrack* x = &a->tracks[i];
        const FigaTrack* y = &b->tracks[i];

        if (x->length != y->length || x->startframe != y->startframe || x->obj_type != y->obj_type ||
            x->frac_value != y->frac_value || x->frac_slope != y->frac_slope || x->dummy0 != y->dummy0)
        {
            return 5;
        }
        if ((x->ad_head == NULL) != (y->ad_head == NULL)) {
            return 6;
        }
        if (x->ad_head != NULL &&
            ((const u8*) x->ad_head - a_file != (const u8*) y->ad_head - b_file ||
             memcmp(x->ad_head, y->ad_head, x->length) != 0))
        {
            return 7;
        }
    }
    return 0;
}

static void report_checks(void)
{
    size_t now, most;

    ps3_dat_pool_usage(&now, &most);
    OSReport("dat: motion check: %u compared, %u of them hits, %u differ; %u parsed in place; %u entries, %u KB; "
             "pool %u KB, at most %u KB\n",
             checks, check_hits, differences, ps3_prof.motion_uncached, nentries, nbytes >> 10, (u32) (now >> 10),
             (u32) (most >> 10));
}

// The tree the game got against a parse of its own buffer, as before the cache.
static void check(const Motion* m, bool hit, u8* src, u32 size, const char* name)
{
    HSD_Archive archive;
    FigaTree* old = NULL;
    int diff;

    if (__real_HSD_ArchiveParse(&archive, src, size) == 0) {
        free_tables(&archive);
        old = lookup(archive.native_data, name);
    }
    diff = old != NULL ? compare(m->tree, m->data, old, src) : 8;
    if (diff != 0) {
        differences++;
        OSReport("dat: motion check %s: differs (%d)\n", name, diff);
    }
    if (archive.native_data != NULL) {
        dat_close(archive.native_data);
    }
    checks++;
    check_hits += hit;
    if ((checks & 255) == 0) {
        report_checks();
    }
}
#endif

static s32 motion(u8* src, u32 size, const char* name, FigaTree** tree, bool keep)
{
    HSD_Archive archive;
    s32 r;

    if (name != NULL) {
        uint64_t hash = hash_words(src, size);
        Motion* m = find(hash, src, size, name);
        bool hit = m != NULL;

        if (hit) {
            unlink_use(m);
            link_newest(m);
            ps3_prof.motion_hits++;
        } else if (nbytes < CACHE_BYTES || ps3_rb_session()) {
            // In a rollback session an in-place parse would free natives that a restored state still uses.
            m = add(hash, src, size, name);
            ps3_prof.motion_misses += m != NULL;
        } else if (!full_logged) {
            full_logged = true;
            OSReport("dat: motion cache full, %u entries, %u KB: parsing in place\n", nentries, nbytes >> 10);
        }
        if (m != NULL) {
            // As a parse of the buffer would: older archives there go, and the GP's caches of it.
            if (!keep) {
                ps3_dat_release_spans(src, size);
            }
            ps3_mem_written(src, size);
            ps3_prof.motion_entries = nentries;
            ps3_prof.motion_bytes = nbytes;
#ifdef DAT_MOTION_CHECK
            check(m, hit, src, size, name);
#endif
            *tree = m->tree;
            return 0;
        }
    }
    ps3_prof.motion_uncached++;
    r = keep ? ps3_dat_parse_keep(&archive, src, size) : HSD_ArchiveParse(&archive, src, size);
    *tree = HSD_ArchiveGetPublicAs(FigaTree, &archive, name);
    return r;
}

s32 ps3_dat_motion(u8* src, u32 size, const char* name, FigaTree** tree)
{
    return motion(src, size, name, tree, false);
}

s32 ps3_dat_motion_keep(u8* src, u32 size, const char* name, FigaTree** tree)
{
    return motion(src, size, name, tree, true);
}

// In a rollback session the in-place parses are kept by buffer, bytes and name: frames that run again parse the
// same motions into the same buffer, and a restored state may still use the natives an older parse would free.
typedef struct InPlace {
    struct InPlace* next;
    const u8* src;
    uint64_t hash;
    u32 size;
    DatArchive* native;
    FigaTree* tree;
    char name[];
} InPlace;

static InPlace* in_place;

static s32 parse_kept(u8* src, u32 size, const char* name, FigaTree** tree)
{
    uint64_t hash = hash_words(src, size);
    size_t len = strlen(name) + 1;
    HSD_Archive archive;
    InPlace* e;

    for (e = in_place; e != NULL; e = e->next) {
        if (e->src == src && e->size == size && e->hash == hash && strcmp(e->name, name) == 0) {
            ps3_mem_written(src, size);
            *tree = e->tree;
            return 0;
        }
    }
    if (__real_HSD_ArchiveParse(&archive, src, size) != 0) {
        return -1;
    }
    free_tables(&archive);
    e = malloc(sizeof(*e) + len);
    if (e == NULL) {
        dat_close(archive.native_data);
        return -1;
    }
    e->src = src;
    e->hash = hash;
    e->size = size;
    e->native = archive.native_data;
    e->tree = lookup(archive.native_data, name);
    memcpy(e->name, name, len);
    e->next = in_place;
    in_place = e;
    ps3_mem_written(src, size);
    *tree = e->tree;
    return 0;
}

s32 ps3_dat_motion_parse(u8* src, u32 size, const char* name, FigaTree** tree)
{
    HSD_Archive archive;
    s32 r;

    if (ps3_rb_session() && name != NULL) {
        return parse_kept(src, size, name, tree);
    }
    r = HSD_ArchiveParse(&archive, src, size);
    *tree = HSD_ArchiveGetPublicAs(FigaTree, &archive, name);
    return r;
}

void ps3_dat_motion_scene(void)
{
    u32 dropped = 0;

    // The last scene's fighters are gone, and with them the buffers the kept in-place parses point into.
    while (in_place != NULL) {
        InPlace* e = in_place;

        in_place = e->next;
        dat_close(e->native);
        free(e);
    }
    while (oldest != NULL && nbytes > SCENE_BYTES) {
        drop(oldest);
        dropped++;
    }
    full_logged = false;
    ps3_prof.motion_entries = nentries;
    ps3_prof.motion_bytes = nbytes;
    if (nentries != 0 || dropped != 0) {
        OSReport("dat: motion cache: %u entries, %u KB; %u dropped\n", nentries, nbytes >> 10, dropped);
    }
#ifdef DAT_MOTION_CHECK
    report_checks();
#endif
}
