// Linker wrappers (-Wl,--wrap) that log the game's archive calls, to find where the main thread stops after
// the memory card scene loads (step 3 diagnostics).
#include <dolphin/os.h>
#include <sysdolphin/baselib/archive.h>

s32 __real_HSD_ArchiveParse(HSD_Archive* archive, u8* src, size_t size);
void* __real_HSD_ArchiveGetPublicTyped(HSD_Archive* archive, const char* name, const char* type);

s32 __wrap_HSD_ArchiveParse(HSD_Archive* archive, u8* src, size_t size)
{
    s32 r;

    OSReport("dat: parse %p, %u bytes\n", src, (u32) size);
    r = __real_HSD_ArchiveParse(archive, src, size);
    OSReport("dat: parse %p -> %d\n", src, r);
    return r;
}

void* __wrap_HSD_ArchiveGetPublicTyped(HSD_Archive* archive, const char* name, const char* type)
{
    void* p;

    OSReport("dat: typed %s as %s\n", name, type);
    p = __real_HSD_ArchiveGetPublicTyped(archive, name, type);
    OSReport("dat: typed %s -> %p\n", name, p);
    return p;
}
