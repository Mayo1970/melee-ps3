// What the rollback's SPU program (rbspu.c) needs from where it runs: an SPU (rbspu_cell.c), or a thread in the host
// builds (tests/rbspu_host.c). Addresses and sizes as MFC DMA takes them: 16-byte aligned, at most 16 KB each.
#ifndef PS3_NET_RBSPU_IO_H
#define PS3_NET_RBSPU_IO_H

#include <stdint.h>

void rbio_get(void* ls, uint32_t ea, uint32_t size);
void rbio_put(const void* ls, uint32_t ea, uint32_t size);
// Waits for every transfer started so far.
void rbio_wait(void);
void rbio_wait_us(uint32_t us);

// The program; shared_ea is the RbShared block's address.
void rb_spu_main(uint32_t shared_ea);

#endif
