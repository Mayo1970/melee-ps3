// A GX test frame of nine cells, through the SDK's GX API: the host test and the hardware EBOOT draw the same.
#ifndef PS3_TESTS_GX_SCENE_H
#define PS3_TESTS_GX_SCENE_H

#include <dolphin/types.h>

typedef struct GxSceneCell {
    const char* name;
    // What the cell shows when it works.
    const char* expect;
} GxSceneCell;

extern const GxSceneCell gx_scene_cells[9];

// arena: 32-byte aligned memory in a ps3_mem_add_region region, for the textures and the XFB.
void gx_scene_init(u8* arena, u32 size);
// Draws frame n (the sphere turns) and copies it to the XFB; returns the XFB's physical address.
u32 gx_scene_frame(u32 n);
// The EFB position of cell i's centre, for the checks.
void gx_scene_cell_center(int i, u32* x, u32* y);

#endif
