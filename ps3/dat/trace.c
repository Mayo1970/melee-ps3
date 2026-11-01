// Linker wrappers (-Wl,--wrap) that log the game's archive calls, to find where the main thread stops after
// the memory card scene loads (step 3 diagnostics).
#include <dolphin/os.h>
#include <sysdolphin/baselib/archive.h>

#include "lookup.h"

void* __real_HSD_ArchiveGetPublicTyped(HSD_Archive* archive, const char* name, const char* type);

// The parse log is in lookup.c, which wraps HSD_ArchiveParse for script pointers.
void* __wrap_HSD_ArchiveGetPublicTyped(HSD_Archive* archive, const char* name, const char* type)
{
    void* p;

    if (DAT_TRACE) {
        OSReport("dat: typed %s as %s\n", name, type);
    }
    p = __real_HSD_ArchiveGetPublicTyped(archive, name, type);
    if (DAT_TRACE || p == NULL) {
        OSReport("dat: typed %s -> %p\n", name, p);
    }
    return p;
}
