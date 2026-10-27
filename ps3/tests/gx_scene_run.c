// The GX test frame on the device layer, for the EBOOT: memory, interrupts, backend, GP thread, GX.
#include <malloc.h>
#include <stdbool.h>

#include <dolphin/gx.h>
#include <dolphin/os.h>

#include "../gx/backend.h"
#include "../gx/fifo.h"
#include "../gx/gp.h"
#include "../os/intr.h"
#include "../os/mem.h"
#include "../os/udplog.h"
#include "gx_scene.h"

#define ARENA_SIZE (24 << 20)
#define FIFO_SIZE 0x40000
#define RING_SIZE (1 << 20)

bool gx_scene_start(void)
{
    u8* arena = memalign(32, ARENA_SIZE);
    int i;

    if (arena == NULL || !ps3_mem_add_region(arena, ARENA_SIZE)) {
        ps3_log("gx_scene: no memory for the arena\n");
        return false;
    }
    ps3_log("gx_scene: arena at %p\n", arena);
    ps3_intr_init();
    ps3_log("gx_scene: interrupt thread started\n");
    if (!gx_rsx_init()) {
        ps3_log("gx_scene: gx_rsx_init failed\n");
        return false;
    }
    ps3_log("gx_scene: backend ready\n");
    gp_init(gx_rsx_backend(), RING_SIZE);
    ps3_log("gx_scene: GP thread started\n");
    GXInit(arena, FIFO_SIZE);
    ps3_log("gx_scene: GXInit done\n");
    gx_scene_init(arena + FIFO_SIZE, ARENA_SIZE - FIFO_SIZE);
    ps3_log("gx_scene: scene ready\n");
    for (i = 0; i < 9; i++) {
        ps3_log("cell %d, %s: %s\n", i + 1, gx_scene_cells[i].name, gx_scene_cells[i].expect);
    }
    return true;
}

void gx_scene_step(unsigned n)
{
    u32 xfb = gx_scene_frame(n);

    if (n < 3) {
        ps3_log("gx_scene: frame %u drawn by GX, XFB %08x\n", n, xfb);
    }
    gp_fifo_sync();
    if (n < 3) {
        ps3_log("gx_scene: frame %u run by the GP\n", n);
    }
    gx_rsx_present(xfb);
    if (n < 3) {
        ps3_log("gx_scene: frame %u presented\n", n);
    }
}

void gx_scene_report(void)
{
    GxRsxStats st;
    GpStats gs;

    gx_rsx_get_stats(&st);
    gp_get_stats(&gs);
    ps3_log("GP: %u draws, %u vertices, %u unknown opcodes; backend: %u dropped draws, %u VPs, %u FPs, %u textures, "
           "%u presents, unsupported %#x\n",
           gs.draws, gs.vertices, gs.unknown_opcodes, st.dropped_draws, st.vertex_programs, st.fragment_programs,
           st.textures, st.presents, st.unsupported);
}
