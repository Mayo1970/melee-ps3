// Vertex conversion on an SPU (vxspu.c): the GP thread queues a draw's GX vertices with the state the conversion
// reads, the SPU writes the RSX vertices into the ring, and every RSX flush (PUT move) waits for the queue first.
#ifndef PS3_GX_VXJOB_H
#define PS3_GX_VXJOB_H

#include "vtx.h"
#include "vx_shared.h"

// Starts the SPU program (vx_start); false leaves the conversion on the PPE.
bool vx_init(void);
// True while the SPU converts: ring outputs then start 16-byte aligned (vx_align).
bool vx_running(void);
// Queues the conversion of count vertices into out (the vertex ring, 16-byte aligned). False when the caller
// converts on the PPE: the SPU is off or the draw is too large.
bool vx_submit(const VtxLayout* l, const GpState* s, const u8* in, u32 count, u8* out);
// Waits until the SPU has written every queued draw. The device calls it before each PUT move.
void vx_drain(void);
// The first vertex at or after verts whose offset is 16-byte aligned, while the SPU converts.
static inline u32 vx_align(u32 verts, u32 out_size)
{
    if (vx_running()) {
        while ((verts * out_size) & 15) {
            verts++;
        }
    }
    return verts;
}

// The platform's start of the program (vxjob_cell.c, tests/vxspu_host.c); weak, so other tests run without it.
bool vx_start(VxShared* shared) __attribute__((weak));

#endif
