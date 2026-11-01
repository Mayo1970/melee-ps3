// The GP backend on the RSX: GX draws through generated vertex and fragment programs, GX textures, EFB copies and
// a virtual XFB (each display copy is an RSX surface, shown by gx_rsx_present).
#ifndef PS3_GX_BACKEND_H
#define PS3_GX_BACKEND_H

#include "gp.h"
#include "rsxdev.h"

// The EFB as the RSX holds it.
#define EFB_WIDTH 640
#define EFB_HEIGHT 528

const GpBackend* gx_rsx_backend(void);
// dev_init and the backend's RSX memory. Call before gp_init.
bool gx_rsx_init(void);
// Shows the display copy made to GameCube physical address xfb. The RSX belongs to the GP thread: call it only
// while that thread is idle (after gp_fifo_sync).
void gx_rsx_present(u32 xfb);
// Whether the system draws over the game (the XMB). Any thread; the presents read it.
void gx_rsx_set_overlay(bool on);

// What the presents show besides the game: the picture at 16:9, and a picture over it (pixels NULL: none).
typedef struct GxRsxScreen {
    bool wide;
    DevOverlay overlay;
} GxRsxScreen;
// fn fills a GxRsxScreen at each present, on the GP thread.
void gx_rsx_set_screen(void (*fn)(GxRsxScreen* out));

typedef struct GxRsxStats {
    u32 draws, vertices, dropped_draws, rsx_draws;
    u32 vertex_programs, fragment_programs, vp_loads;
    u32 textures, texture_uploads, efb_copies, xfb_copies, presents;
    u32 unsupported; // bit per feature the backend does not draw (see backend.c)
} GxRsxStats;
void gx_rsx_get_stats(GxRsxStats* out);

#endif
