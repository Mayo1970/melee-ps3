// The Wii U GameCube adapter through PSL1GHT's usbd. See gcadapter.h.
#include "gcadapter.h"

#include <stdint.h>
#include <string.h>

#include <ppu-asm.h>
#include <sysmodule/sysmodule.h>
#include <usb/usb.h>

#include "../os/sync.h"
#include "../os/udplog.h"

#define VENDOR 0x057E
#define PRODUCT 0x0337
#define REPORT_SIZE 37
#define PORT_SIZE 9
#define INPUT_REPORT 0x21
#define RUMBLE_REPORT 0x11
// The adapter sends no reports before it gets this.
#define INIT_COMMAND 0x13

// The ops as the PRX reads them: 32-bit addresses, 32-bit OPDs.
typedef struct LddOps32 {
    u32 name, probe, attach, detach;
} LddOps32;

// PSL1GHT's usbRegisterExtraLdd hands the PRX a copy on its stack, which the PRX may keep: ours is static.
s32 usbRegisterExtraLddEx(LddOps32* ops, u16 vendor_id, u16 product_id);

static PsMutex lock = PS_MUTEX_INIT;
static LddOps32 ops;
static const char ldd_name[] = "gcadapter";
// Each attach starts a new generation; callbacks of an older one do nothing.
static unsigned gen;
static s32 dev = -1, in_pipe = -1, out_pipe = -1;
static unsigned char* in_buf;
static unsigned char* out_buf;
static unsigned char static_in[64] __attribute__((aligned(128)));
static unsigned char static_out[64] __attribute__((aligned(128)));
static unsigned char report[REPORT_SIZE];
static bool started, out_busy;
static unsigned rumble_want, rumble_sent;

// The PRX gives 32-bit addresses.
static void* prx_ptr(void* p)
{
    return (void*) (uintptr_t) (u32) (uintptr_t) p;
}

static u32 ldd_opd(s32 (*fn)(s32))
{
    return (u32) __get_opd32(fn);
}

static void* transfer_buffer(unsigned char* fallback, size_t size)
{
    u32 addr = 0;

    if (usbAllocateMemory((void**) &addr, size) != 0 || addr == 0) {
        ps3_log("gcadapter: usbAllocateMemory failed, static buffer\n");
        return fallback;
    }
    return (void*) (uintptr_t) addr;
}

static void on_in(s32 result, s32 count, void* arg);
static void on_out(s32 result, s32 count, void* arg);

static void submit_in(unsigned g)
{
    s32 pipe, err;

    ps_mutex_lock(&lock);
    pipe = g == gen ? in_pipe : -1;
    ps_mutex_unlock(&lock);
    if (pipe >= 0 && (err = usbInterruptTransfer(pipe, in_buf, REPORT_SIZE, on_in, (void*) (uintptr_t) g)) != 0) {
        ps3_log("gcadapter: input transfer failed: %d\n", (int) err);
    }
}

// Sends the rumble state when it changed and no output transfer is under way.
static void pump_out(unsigned g)
{
    s32 pipe = -1;
    int i;

    ps_mutex_lock(&lock);
    if (g == gen && started && !out_busy && rumble_want != rumble_sent) {
        out_buf[0] = RUMBLE_REPORT;
        for (i = 0; i < PS3_GCADAPTER_PORTS; i++) {
            out_buf[1 + i] = (rumble_want >> i) & 1;
        }
        rumble_sent = rumble_want;
        out_busy = true;
        pipe = out_pipe;
    }
    ps_mutex_unlock(&lock);
    if (pipe >= 0 && usbInterruptTransfer(pipe, out_buf, 1 + PS3_GCADAPTER_PORTS, on_out, (void*) (uintptr_t) g) != 0)
    {
        ps_mutex_lock(&lock);
        out_busy = false;
        ps_mutex_unlock(&lock);
    }
}

static void on_in(s32 result, s32 count, void* arg)
{
    unsigned g = (unsigned) (uintptr_t) arg;

    ps_mutex_lock(&lock);
    if (g != gen) {
        ps_mutex_unlock(&lock);
        return;
    }
    if (result == USB_HC_CC_NOERR && count == REPORT_SIZE && in_buf[0] == INPUT_REPORT) {
        memcpy(report, in_buf, REPORT_SIZE);
    }
    ps_mutex_unlock(&lock);
    if (result != USB_HC_CC_NOERR) {
        ps3_log("gcadapter: input error %d, reads stop\n", (int) result);
        return;
    }
    submit_in(g);
}

// The first output transfer is the init command; the others carry rumble.
static void on_out(s32 result, s32 count, void* arg)
{
    unsigned g = (unsigned) (uintptr_t) arg;
    bool first;

    ps_mutex_lock(&lock);
    if (g != gen) {
        ps_mutex_unlock(&lock);
        return;
    }
    out_busy = false;
    first = !started;
    if (first && result == USB_HC_CC_NOERR) {
        started = true;
    }
    ps_mutex_unlock(&lock);
    if (first) {
        ps3_log("gcadapter: init command: result %d\n", (int) result);
        if (result == USB_HC_CC_NOERR) {
            submit_in(g);
        }
    }
    pump_out(g);
}

static void on_config(s32 result, s32 count, void* arg)
{
    unsigned g = (unsigned) (uintptr_t) arg;
    usbEndpointDescriptor* e = NULL;
    s32 d, in = -1, out = -1, err;

    ps_mutex_lock(&lock);
    d = g == gen ? dev : -1;
    ps_mutex_unlock(&lock);
    if (d < 0) {
        return;
    }
    if (result != USB_HC_CC_NOERR) {
        ps3_log("gcadapter: set configuration failed: %d\n", (int) result);
        return;
    }
    while ((e = prx_ptr(usbScanStaticDescriptor(d, e, USB_DESCRIPTOR_TYPE_ENDPOINT))) != NULL) {
        if ((e->bmAttributes & USB_ENDPOINT_TRANSFER_TYPE_BITS) != USB_ENDPOINT_TRANSFER_TYPE_INTERRUPT) {
            continue;
        }
        if (e->bEndpointAddress & USB_ENDPOINT_DIRECTION_IN) {
            if (in < 0) {
                in = usbOpenPipe(d, e);
            }
        } else if (out < 0) {
            out = usbOpenPipe(d, e);
        }
    }
    ps3_log("gcadapter: pipes in %d out %d\n", (int) in, (int) out);
    if (in < 0 || out < 0) {
        return;
    }
    ps_mutex_lock(&lock);
    if (g == gen) {
        in_pipe = in;
        out_pipe = out;
        out_busy = true;
        out_buf[0] = INIT_COMMAND;
    }
    ps_mutex_unlock(&lock);
    if ((err = usbInterruptTransfer(out, out_buf, 1, on_out, (void*) (uintptr_t) g)) != 0) {
        ps3_log("gcadapter: init command failed: %d\n", (int) err);
    }
}

static s32 probe(s32 dev_id)
{
    return USB_PROBE_SUCCEEDED;
}

static s32 attach(s32 dev_id)
{
    usbConfigDescriptor* config;
    s32 ctrl;
    unsigned g;

    ps_mutex_lock(&lock);
    if (dev >= 0) {
        ps_mutex_unlock(&lock);
        ps3_log("gcadapter: a second adapter is ignored\n");
        return USB_ATTACH_FAILED;
    }
    dev = dev_id;
    g = ++gen;
    in_pipe = out_pipe = -1;
    started = out_busy = false;
    rumble_sent = 0;
    memset(report, 0, sizeof(report));
    ps_mutex_unlock(&lock);

    config = prx_ptr(usbScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_CONFIG));
    ctrl = usbOpenPipe(dev_id, NULL);
    ps3_log("gcadapter: attach %d, control pipe %d\n", (int) dev_id, (int) ctrl);
    if (config == NULL || ctrl < 0 ||
        usbSetConfiguration(ctrl, config->bConfigurationValue, on_config, (void*) (uintptr_t) g) != 0)
    {
        ps3_log("gcadapter: attach failed\n");
        ps_mutex_lock(&lock);
        dev = -1;
        ps_mutex_unlock(&lock);
        return USB_ATTACH_FAILED;
    }
    return USB_ATTACH_SUCCEEDED;
}

static s32 detach(s32 dev_id)
{
    ps_mutex_lock(&lock);
    if (dev_id == dev) {
        dev = -1;
        gen++;
        in_pipe = out_pipe = -1;
        started = false;
        memset(report, 0, sizeof(report));
    }
    ps_mutex_unlock(&lock);
    ps3_log("gcadapter: detach %d\n", (int) dev_id);
    return USB_DETACH_SUCCEEDED;
}

void ps3_gcadapter_init(void)
{
    s32 err;

    if ((err = sysModuleLoad(SYSMODULE_USB)) != 0 || (err = usbInit()) != 0) {
        ps3_log("gcadapter: usbd init failed: %d\n", (int) err);
        return;
    }
    in_buf = transfer_buffer(static_in, sizeof(static_in));
    out_buf = transfer_buffer(static_out, sizeof(static_out));
    ops.name = __get_addr32(ldd_name);
    ops.probe = ldd_opd(probe);
    ops.attach = ldd_opd(attach);
    ops.detach = ldd_opd(detach);
    err = usbRegisterExtraLddEx(&ops, VENDOR, PRODUCT);
    ps3_log("gcadapter: driver registered: %d\n", (int) err);
}

void ps3_gcadapter_read(Ps3GcPad pads[PS3_GCADAPTER_PORTS])
{
    int i;

    memset(pads, 0, sizeof(*pads) * PS3_GCADAPTER_PORTS);
    ps_mutex_lock(&lock);
    for (i = 0; i < PS3_GCADAPTER_PORTS; i++) {
        const unsigned char* p = &report[1 + i * PORT_SIZE];

        // The high status nibble is the pad type: 1 wired, 2 wireless, 0 none.
        if (report[0] != INPUT_REPORT || (p[0] & 0x30) == 0) {
            continue;
        }
        pads[i].connected = true;
        pads[i].buttons = p[1] | (unsigned) p[2] << 8;
        pads[i].sx = p[3];
        pads[i].sy = p[4];
        pads[i].cx = p[5];
        pads[i].cy = p[6];
        pads[i].l = p[7];
        pads[i].r = p[8];
    }
    ps_mutex_unlock(&lock);
}

void ps3_gcadapter_rumble(int port, bool on)
{
    unsigned g;

    if (port < 0 || port >= PS3_GCADAPTER_PORTS) {
        return;
    }
    ps_mutex_lock(&lock);
    rumble_want = on ? rumble_want | 1u << port : rumble_want & ~(1u << port);
    g = gen;
    ps_mutex_unlock(&lock);
    pump_out(g);
}
