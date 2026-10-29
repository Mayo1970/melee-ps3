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

#endif
