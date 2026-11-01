// What the port's .dat sources share: the trace switch and lookup.c's parsed archives.
#ifndef PS3_DAT_LOOKUP_H
#define PS3_DAT_LOOKUP_H

#include <stddef.h>

#include <dolphin/types.h>

// A log line for every parse and lookup with -DPS3_DAT_TRACE (step 3 diagnostics); failures always log.
#ifdef PS3_DAT_TRACE
#define DAT_TRACE 1
#else
#define DAT_TRACE 0
#endif

// Frees the native data of the parsed archives whose buffers overlap the range, as a parse there does.
void ps3_dat_release_spans(const void* start, size_t size);
struct HSD_Archive;
// HSD_ArchiveParse that keeps the archives it overlaps: their natives may still be in use.
s32 ps3_dat_parse_keep(struct HSD_Archive* archive, u8* src, size_t size);

#endif
