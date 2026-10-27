// The GX test EBOOT: draws the GX test frame (tests/gx_scene.c) on the RSX until the XMB asks to quit. PSL1GHT
// side only: the GX side is in tests/gx_scene_run.c, as no SDK header may meet a PSL1GHT one.
#include <stdbool.h>
#include <stdio.h>

#include <sys/process.h>
#include <sysutil/sysutil.h>

#include "os/udplog.h"

SYS_PROCESS_PARAM(1001, 0x100000);

bool gx_scene_start(void);
void gx_scene_step(unsigned n);
void gx_scene_report(void);

static volatile bool quit;

static void on_sysutil(u64 status, u64 param, void* user)
{
    if (status == SYSUTIL_EXIT_GAME) {
        quit = true;
    }
}

int main(void)
{
    unsigned n;

    ps3_log_init();
    ps3_log("gx_test: boot\n");
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, on_sysutil, NULL);
    if (!gx_scene_start()) {
        ps3_log("gx_test: start failed\n");
        return 1;
    }
    for (n = 0; !quit; n++) {
        gx_scene_step(n);
        sysUtilCheckCallback();
        if (n % 60 == 0) {
            gx_scene_report();
        }
    }
    gx_scene_report();
    ps3_log("gx_test: quit after %u frames\n", n);
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    return 0;
}
