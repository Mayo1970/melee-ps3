// Untyped archive lookups as typed ones (-Wl,--wrap=HSD_ArchiveGetPublicAddress). Upstream's adapter answers
// them with the serialized bytes; the lbArchive loaders use them, and with LP64 they need native objects.
#include <stdlib.h>
#include <string.h>

#include "melee_dat.h"
#include <dat/reader-internal.h>
#include <dolphin/os.h>
#include <sysdolphin/baselib/archive.h>

#include "../os/mem.h"
#include "../os/sync.h"
#include "pool.h"

void* __real_HSD_ArchiveGetPublicAddress(HSD_Archive* archive, const char* name);

typedef struct Entry {
    const char* name;
    const DatFileRoots* file;
    const DatRoot* root;
} Entry;

// The schema's public roots, sorted by name.
static Entry* entries;
static uint32_t nentries;

static int compare(const void* a, const void* b)
{
    return strcmp(((const Entry*) a)->name, ((const Entry*) b)->name);
}

static void build(void)
{
    uint32_t i, j, k, n = 0;

    for (i = 0; i < melee_dat_schema.nmodules; i++) {
        for (j = 0; j < melee_dat_schema.modules[i].nfiles; j++) {
            n += melee_dat_schema.modules[i].files[j].nroots;
        }
    }
    entries = malloc(sizeof(*entries) * (n ? n : 1));
    if (entries == NULL) {
        abort();
    }
    for (i = 0; i < melee_dat_schema.nmodules; i++) {
        for (j = 0; j < melee_dat_schema.modules[i].nfiles; j++) {
            const DatFileRoots* file = &melee_dat_schema.modules[i].files[j];

            for (k = 0; k < file->nroots; k++) {
                if (!file->roots[k].alias) {
                    entries[nentries].name = file->roots[k].name;
                    entries[nentries].file = file;
                    entries[nentries].root = &file->roots[k];
                    nentries++;
                }
            }
        }
    }
    qsort(entries, nentries, sizeof(*entries), compare);
}

// The first entry for name, or NULL.
static const Entry* find(const char* name)
{
    uint32_t lo = 0, hi = nentries;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;

        if (strcmp(entries[mid].name, name) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < nentries && strcmp(entries[lo].name, name) == 0 ? &entries[lo] : NULL;
}

static int archive_has(const DatArchiveData* raw, const char* name)
{
    uint32_t i;

    for (i = 0; i < raw->npublics; i++) {
        if (strcmp(raw->publics[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

// The root's type. A name some files type differently (stage `yakumono_param`) takes the type of the file
// whose public roots the archive has the most of.
static int32_t root_type(const DatArchive* native, const char* name)
{
    const Entry* first = find(name);
    const Entry* end = entries + nentries;
    const Entry* best = first;
    const Entry* e;
    int best_score = -1;

    if (first == NULL) {
        return DAT_NONE;
    }
    for (e = first; e < end && strcmp(e->name, name) == 0 && e->root->type == first->root->type; e++) {
    }
    if (e == end || strcmp(e->name, name) != 0) {
        return first->root->type;
    }
    for (e = first; e < end && strcmp(e->name, name) == 0; e++) {
        int score = 0;
        uint32_t k;

        for (k = 0; k < e->file->nroots; k++) {
            score += !e->file->roots[k].alias && archive_has(native->archive, e->file->roots[k].name);
        }
        if (score > best_score) {
            best_score = score;
            best = e;
        }
    }
    return best->root->type;
}

void* __wrap_HSD_ArchiveGetPublicAddress(HSD_Archive* archive, const char* name)
{
    const DatType* t;
    int32_t type;

    if (archive == NULL || archive->native_data == NULL || name == NULL) {
        return __real_HSD_ArchiveGetPublicAddress(archive, name);
    }
    if (entries == NULL) {
        build();
    }
    type = root_type(archive->native_data, name);
    t = type != DAT_NONE ? dat_reader_T(archive->native_data, dat_reader_resolve(archive->native_data, type))
                         : NULL;
    // Data without pointers and in the same layout is the same bytes on the PS3, which is big-endian too.
    if (t == NULL || t->kind == DAT_KIND_VOID || (!t->has_pointers && t->native_size == t->size)) {
        void* raw = __real_HSD_ArchiveGetPublicAddress(archive, name);

        // Step 3 diagnostics, as the two lines below.
        OSReport("dat: public %s, serialized -> %p\n", name, raw);
        return raw;
    }
    {
        void* native = dat_public(archive->native_data, name, type);

        OSReport("dat: public %s as %s -> %p\n", name, t->name, native);
        return native;
    }
}

void* ps3_dat_element(HSD_Archive* archive, const void* native, const char* type_name, int index)
{
    DatArchive* a;
    const DatMap* natives;
    int32_t type;
    size_t i;

    if (archive == NULL || archive->native_data == NULL || native == NULL) {
        return NULL;
    }
    a = archive->native_data;
    type = melee_dat_type(type_name);
    if (type == DAT_NONE) {
        return NULL;
    }
    natives = &a->natives;
    // The natives map goes from (offset, type id) to the object; the reverse takes a scan.
    for (i = 0; i < natives->cap; i++) {
        if (natives->keys[i] != UINT64_MAX && natives->values[i] == (uintptr_t) native) {
            uint32_t offset = (uint32_t) (natives->keys[i] >> 32);
            uint32_t size = dat_reader_T(a, dat_reader_resolve(a, type))->size;

            return dat_at(a, offset + (uint32_t) index * size, type, DAT_COUNT_ONE, 1);
        }
    }
    OSReport("dat: element %d of %s %p: not an object of the archive\n", index, type_name, native);
    return NULL;
}

// Each parsed archive's buffer and native data, newest last. The GameCube parses in place and the game reuses
// buffers without a release (fighter motions), so a parse frees the native data of the archives it overlaps.
#define MAX_SPANS 1024

typedef struct Span {
    uintptr_t start, end;
    // The native reader's copy of the archive's data, which holds the scripts.
    u8* data;
    u32 data_size;
    DatArchive* native;
    void* infos[3];
} Span;

static Span spans[MAX_SPANS];
static unsigned nspans;
static PsMutex spans_lock = PS_MUTEX_INIT;

static void release(const Span* sp)
{
    unsigned i;

    dat_close(sp->native);
    for (i = 0; i < 3; i++) {
        free(sp->infos[i]);
    }
}

s32 __real_HSD_ArchiveParse(HSD_Archive* archive, u8* src, size_t size);
void __real_HSD_ArchiveReleaseNative(HSD_Archive* archive);

s32 __wrap_HSD_ArchiveParse(HSD_Archive* archive, u8* src, size_t size)
{
    uintptr_t start = (uintptr_t) src, end = start + size;
    unsigned i, k = 0;
    s32 r;

    ps_mutex_lock(&spans_lock);
    for (i = 0; i < nspans; i++) {
        if (spans[i].end <= start || spans[i].start >= end) {
            spans[k++] = spans[i];
        } else {
            release(&spans[i]);
        }
    }
    nspans = k;
    ps_mutex_unlock(&spans_lock);
    // Step 3 diagnostics, as the line below.
    OSReport("dat: parse %p, %u bytes\n", src, (u32) size);
    r = __real_HSD_ArchiveParse(archive, src, size);
    ps3_mem_written(src, (u32) size);
    {
        size_t now, most;

        ps3_dat_pool_usage(&now, &most);
        OSReport("dat: parse %p -> %d; pool %u KB, at most %u KB\n", src, r, (u32) (now >> 10), (u32) (most >> 10));
    }
    if (r == 0) {
        const DatArchiveData* raw = archive->native_data->archive;
        Span sp = { start, end, raw->data, raw->size, archive->native_data,
                    { archive->reloc_info, archive->public_info, archive->extern_info } };

        ps_mutex_lock(&spans_lock);
        // The oldest one is no longer tracked, and not freed: it may still be in use.
        if (nspans == MAX_SPANS) {
            memmove(&spans[0], &spans[1], sizeof(spans[0]) * --nspans);
        }
        spans[nspans++] = sp;
        ps_mutex_unlock(&spans_lock);
    }
    return r;
}

void __wrap_HSD_ArchiveReleaseNative(HSD_Archive* archive)
{
    unsigned i;

    if (archive != NULL && archive->native_data != NULL) {
        ps_mutex_lock(&spans_lock);
        for (i = 0; i < nspans; i++) {
            if (spans[i].native == archive->native_data) {
                memmove(&spans[i], &spans[i + 1], sizeof(spans[0]) * (nspans - i - 1));
                nspans--;
                break;
            }
        }
        ps_mutex_unlock(&spans_lock);
    }
    __real_HSD_ArchiveReleaseNative(archive);
}

void* ps3_dat_script(const void* at, u32 offset)
{
    uintptr_t p = (uintptr_t) at;
    void* target = NULL;
    unsigned i;

    ps_mutex_lock(&spans_lock);
    for (i = nspans; i-- > 0;) {
        if (p - (uintptr_t) spans[i].data < spans[i].data_size) {
            target = spans[i].data + offset;
            break;
        }
    }
    ps_mutex_unlock(&spans_lock);
    if (target == NULL) {
        OSPanic(__FILE__, __LINE__, "script pointer at %p is in no archive\n", at);
    }
    return target;
}

bool ps3_dat_word(const void* native, u32 offset, u32* out)
{
    bool found = false;
    unsigned i;
    size_t k;

    if (native == NULL) {
        return false;
    }
    ps_mutex_lock(&spans_lock);
    for (i = nspans; i-- > 0 && !found;) {
        const DatMap* natives = &spans[i].native->natives;

        // The natives map goes from (offset, type id) to the object; the reverse takes a scan.
        for (k = 0; k < natives->cap; k++) {
            if (natives->keys[k] != UINT64_MAX && natives->values[k] == (uintptr_t) native) {
                u32 at = (u32) (natives->keys[k] >> 32) + offset;

                if (at + 4 <= spans[i].data_size) {
                    const u8* p = spans[i].data + at;

                    *out = (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
                    found = true;
                }
                break;
            }
        }
    }
    ps_mutex_unlock(&spans_lock);
    return found;
}
