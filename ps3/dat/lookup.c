// Untyped archive lookups as typed ones (linked with -Wl,--wrap=HSD_ArchiveGetPublicAddress). Upstream's adapter
// answers them with the serialized bytes; the lbArchive loaders use them, and with LP64 they need native objects.
#include <stdlib.h>
#include <string.h>

#include "melee_dat.h"
#include <dat/reader-internal.h>
#include <dolphin/os.h>
#include <sysdolphin/baselib/archive.h>

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
