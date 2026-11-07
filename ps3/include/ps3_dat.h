// What the game needs from the port's .dat layer (ps3/dat), under TARGET_PS3.
#ifndef PS3_DAT_H
#define PS3_DAT_H

#include <sysdolphin/baselib/archive.h>

// For code that indexes a struct in place in the archive (`&aj[i]`): the native object `index` elements of
// `type_name` after `native` in the serialized data. Natively each object is allocated on its own.
void* ps3_dat_element(HSD_Archive* archive, const void* native, const char* type_name, int index);

// A command script's pointer word: the native readers keep scripts as serialized bytes, so the word is an
// offset into the data of the archive that holds `at`, the script.
void* ps3_dat_script(const void* at, u32 offset);

// The big-endian word at byte `offset` of the serialized object the reader made `native` from, for code that reads
// a field as another type than its declared one. False when `native` is no object of a live archive.
bool ps3_dat_word(const void* native, u32 offset, u32* out);

// The serialized bytes the reader made `native` from, and in `size` how many the archive has from there. NULL when
// `native` is no object of a live archive. Pointer words in them are archive offsets.
const void* ps3_dat_bytes(const void* native, u32* size);

struct FigaTree;

// A fighter motion's archive, which the game copied to `src`: parses it and looks up `name`, as HSD_ArchiveParse
// and HSD_ArchiveGetPublicAs(FigaTree) do, from a cache of the motions seen before. The parse's result.
s32 ps3_dat_motion(u8* src, u32 size, const char* name, struct FigaTree** tree);
// The same, parsed in the buffer itself, so the motion's FObjs read the buffer as on the GameCube.
s32 ps3_dat_motion_parse(u8* src, u32 size, const char* name, struct FigaTree** tree);
// ps3_dat_motion into a buffer whose earlier archive may still be in use: that archive is kept.
s32 ps3_dat_motion_keep(u8* src, u32 size, const char* name, struct FigaTree** tree);

// A scene's fighters are about to be made and the last scene's are gone: the motion cache makes room.
void ps3_dat_motion_scene(void);

#endif
