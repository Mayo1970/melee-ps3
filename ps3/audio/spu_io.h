#ifndef PS3_AUDIO_SPU_IO_H
#define PS3_AUDIO_SPU_IO_H

// What the audio program needs from where it runs: an SPU, or a thread in the host test.
#include <stdint.h>

// DMA with 16-byte aligned addresses and sizes; transfers over 16 KB are split.
void io_get(void* ls, uint64_t ea, uint32_t size);
void io_put(const void* ls, uint64_t ea, uint32_t size);
int io_mail_count(void);
uint32_t io_mail_read(void);
void io_event(uint32_t code);
void io_wait_us(uint32_t us);

// The program; arg is the effective address of the AxShared block.
int ax_spu_main(uint64_t arg);

#endif
