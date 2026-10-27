// Host test (qemu-ppc64) of the RSX backend: the GX test frame through the GP thread, the backend and the software
// RSX. Writes the presented frames as PPM files and checks a few pixels.
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include <dolphin/gx.h>
#include <dolphin/os.h>

#include "../gx/backend.h"
#include "../gx/fifo.h"
#include "../gx/gp.h"
#include "../os/intr.h"
#include "../os/mem.h"
#include "gx_scene.h"

#define ARENA_SIZE (24 << 20)
#define FIFO_SIZE 0x40000

void soft_set_dump(const char* prefix);
const u32* soft_screen(void);
extern u32 soft_draws, soft_pixels;

static int failures;

// Expects the screen pixel at (x, y) within tol of rgb.
static void expect(const char* what, u32 x, u32 y, u32 rgb, int tol)
{
    u32 p = soft_screen()[y * 640 + x];
    int k;

    for (k = 0; k < 3; k++) {
        int a = (int) ((p >> (16 - 8 * k)) & 0xFF), b = (int) ((rgb >> (16 - 8 * k)) & 0xFF);

        if (a - b > tol || b - a > tol) {
            printf("FAIL %s: pixel (%u, %u) is %06x, want %06x\n", what, x, y, p & 0xFFFFFF, rgb);
            failures++;
            return;
        }
    }
}

int main(int argc, char** argv)
{
    u8* arena = memalign(32, ARENA_SIZE);
    GxRsxStats st;
    GpStats gs;
    u32 xfb = 0, n;

    if (arena == NULL || !ps3_mem_add_region(arena, ARENA_SIZE)) {
        printf("FAIL: arena\n");
        return 1;
    }
    ps3_intr_init();
    if (!gx_rsx_init()) {
        printf("FAIL: gx_rsx_init\n");
        return 1;
    }
    soft_set_dump(argc > 1 ? argv[1] : "scene_");
    gp_init(gx_rsx_backend(), 1 << 20);
    GXInit(arena, FIFO_SIZE);
    gx_scene_init(arena + FIFO_SIZE, ARENA_SIZE - FIFO_SIZE);
    for (n = 0; n < 2; n++) {
        xfb = gx_scene_frame(n * 10);
        gp_fifo_sync();
        gx_rsx_present(xfb);
    }
    gx_rsx_get_stats(&st);
    gp_get_stats(&gs);
    printf("GP: %u draws, %u vertices, %u unknown opcodes\n", gs.draws, gs.vertices, gs.unknown_opcodes);
    printf("backend: %u draws (%u dropped), %u VPs, %u FPs, %u textures, %u EFB copies, %u XFB copies, %u presents, "
           "unsupported %#x\n",
           st.draws, st.dropped_draws, st.vertex_programs, st.fragment_programs, st.textures, st.efb_copies,
           st.xfb_copies, st.presents, st.unsupported);
    printf("software RSX: %u draws, %u pixels\n", soft_draws, soft_pixels);
    // Cell 1: the checker's top-left square is red.
    expect("checker", 228, 28, 0xFF0000, 8);
    // Cell 2: the right of the gradient is green only.
    expect("I8 x konst", 600, 82, 0x00F0C0 & 0x00FF00, 40);
    // Cell 6: red at half alpha over blue.
    expect("blend", 10 + 0.45 * 200, 320 + 0.3 * 145, 0x800080, 6);
    // Cell 8: the clockwise triangle shows, the other is culled.
    expect("cull keeps", 430 + 0.12 * 200, 320 + 0.88 * 145, 0xFFFF00, 8);
    expect("cull drops", 430 + 0.12 * 200, 320 + 0.14 * 145, 0x181830, 8);
    if (st.presents != 2 || st.dropped_draws != 0 || gs.unknown_opcodes != 0) {
        printf("FAIL: presents %u, dropped draws %u\n", st.presents, st.dropped_draws);
        failures++;
    }
    printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures != 0;
}
