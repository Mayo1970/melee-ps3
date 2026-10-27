// spu_io on a real SPU: MFC DMA, the inbound mailbox, and events to the PPU's queue.
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_event.h>
#include <sys/spu_thread.h>

#include "ax_shared.h"
#include "spu_io.h"

#define TAG 1
#define MAX_DMA 16384
// The decrementer counts at the 79.8 MHz timebase.
#define TICKS_PER_US 80

static void dma(void* ls, uint64_t ea, uint32_t size, int put)
{
    uint8_t* p = ls;

    while (size != 0) {
        uint32_t n = size < MAX_DMA ? size : MAX_DMA;
        if (put) {
            mfc_put(p, ea, n, TAG, 0, 0);
        } else {
            mfc_get(p, ea, n, TAG, 0, 0);
        }
        p += n;
        ea += n;
        size -= n;
    }
    mfc_write_tag_mask(1 << TAG);
    mfc_read_tag_status_all();
}

void io_get(void* ls, uint64_t ea, uint32_t size)
{
    dma(ls, ea, size, 0);
}

void io_put(const void* ls, uint64_t ea, uint32_t size)
{
    dma((void*) ls, ea, size, 1);
}

int io_mail_count(void)
{
    return (int) spu_stat_in_mbox();
}

uint32_t io_mail_read(void)
{
    return spu_read_in_mbox();
}

void io_event(uint32_t code)
{
    spu_thread_send_event(AX_EVENT_PORT, code, 0);
}

void io_wait_us(uint32_t us)
{
    uint32_t start = spu_read_decrementer();

    while (start - spu_read_decrementer() < us * TICKS_PER_US) {
    }
}

int main(uint64_t arg)
{
    spu_write_decrementer(0xFFFFFFFF);
    ax_spu_main(arg);
    spu_thread_exit(0);
    return 0;
}
