// What the game needs from the port's .dat layer (ps3/dat), under TARGET_PS3.
#ifndef PS3_DAT_H
#define PS3_DAT_H

#include <sysdolphin/baselib/archive.h>

// For code that indexes a struct in place in the archive (`&aj[i]`): the native object `index` elements of
// `type_name` after `native` in the serialized data. Natively each object is allocated on its own.
void* ps3_dat_element(HSD_Archive* archive, const void* native, const char* type_name, int index);

#endif
