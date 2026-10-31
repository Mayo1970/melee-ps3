// What the vertex program (vxspu.c) needs from where it runs: an SPU (vxspu_cell.c), or a thread in the host
// builds (tests/vxspu_host.c). Each call waits until its transfer is done.
#ifndef PS3_GX_VXSPU_IO_H
#define PS3_GX_VXSPU_IO_H

#include <stdint.h>

// One window of a gather: the MFC list element's layout (size below 32 KB, then the address).
typedef struct VxGather {
    uint32_t size;
    uint32_t ea;
} VxGather;

// 16-byte aligned addresses, sizes a multiple of 16; larger than 16 KB is split.
void vxio_get(void* ls, uint32_t ea, uint32_t size);
void vxio_put(const void* ls, uint32_t ea, uint32_t size);
// The n windows of list, one after the other from ls; at most 2048.
void vxio_gather(void* ls, const VxGather* list, uint32_t n);
void vxio_wait_us(uint32_t us);

// The program; shared_ea is the VxShared block's address.
void vx_spu_main(uint32_t shared_ea);

#endif
