// vxspu_io on a real SPU: MFC DMA and list DMA (spu-gcc only).
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#include "vxspu_io.h"

#define TAG 1
#define MAX_DMA 16384
// The decrementer counts at the 79.8 MHz timebase.
#define TICKS_PER_US 80

static void dma(void* ls, uint32_t ea, uint32_t size, int put)
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

void vxio_get(void* ls, uint32_t ea, uint32_t size)
{
    dma(ls, ea, size, 0);
}

void vxio_put(const void* ls, uint32_t ea, uint32_t size)
{
    dma((void*) ls, ea, size, 1);
}

void vxio_gather(void* ls, const VxGather* list, uint32_t n)
{
    // VxGather has the layout of mfc_list_element_t; every address is below 4 GB.
    mfc_getl(ls, 0, list, n * sizeof(VxGather), TAG, 0, 0);
    mfc_write_tag_mask(1 << TAG);
    mfc_read_tag_status_all();
}

void vxio_wait_us(uint32_t us)
{
    uint32_t start = spu_read_decrementer();

    while (start - spu_read_decrementer() < us * TICKS_PER_US) {
    }
}

int main(uint64_t arg)
{
    spu_write_decrementer(0xFFFFFFFF);
    vx_spu_main((uint32_t) arg);
    spu_thread_exit(0);
    return 0;
}
