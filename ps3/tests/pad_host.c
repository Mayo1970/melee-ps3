// The pads for the host game build (see pad_cell.h): none, or pad 0 playing the MELEE_HOST_PAD script.
#include "../pad/pad_cell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/vi.h>

#include "../os/diag.h"
#include "../os/udplog.h"

#define MAX_STEPS 256

typedef struct Step {
    u32 at, length;
    unsigned buttons;
    unsigned char l2, r2, lx, ly, rx, ry;
} Step;

static Step steps[MAX_STEPS];
static int nsteps;
static bool scripted;

// The GameCube buttons, as pad.c maps them, SL SR SU SD for the main stick, CL CR CU CD for the C stick.
static bool parse_input(const char* name, Step* s)
{
    static const struct {
        const char* name;
        unsigned buttons;
    } names[] = {
        { "A", PS3_PAD_CROSS },   { "B", PS3_PAD_SQUARE },  { "X", PS3_PAD_CIRCLE },  { "Y", PS3_PAD_TRIANGLE },
        { "Z", PS3_PAD_R1 },      { "START", PS3_PAD_START }, { "UP", PS3_PAD_UP },   { "DOWN", PS3_PAD_DOWN },
        { "LEFT", PS3_PAD_LEFT }, { "RIGHT", PS3_PAD_RIGHT }, { "SELECT", PS3_PAD_SELECT },
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i].name) == 0) {
            s->buttons |= names[i].buttons;
            return true;
        }
    }
    // L and R are the analog L2 and R2, pressed past the click.
    if (strcmp(name, "L") == 0) {
        s->buttons |= PS3_PAD_L2;
        s->l2 = 255;
    } else if (strcmp(name, "R") == 0) {
        s->buttons |= PS3_PAD_R2;
        s->r2 = 255;
    } else if (strcmp(name, "SL") == 0) {
        s->lx = 0;
    } else if (strcmp(name, "SR") == 0) {
        s->lx = 255;
    } else if (strcmp(name, "SU") == 0) {
        s->ly = 0;
    } else if (strcmp(name, "SD") == 0) {
        s->ly = 255;
    } else if (strcmp(name, "CL") == 0) {
        s->rx = 0;
    } else if (strcmp(name, "CR") == 0) {
        s->rx = 255;
    } else if (strcmp(name, "CU") == 0) {
        s->ry = 0;
    } else if (strcmp(name, "CD") == 0) {
        s->ry = 255;
    } else {
        return false;
    }
    return true;
}

// Comma-separated "retrace:inputs[:retraces]", inputs joined by '+', held 6 retraces by default.
// Example: MELEE_HOST_PAD=600:START,700:A,760:SD:4.
static void parse(const char* script)
{
    char buf[4096];
    char* save = NULL;
    char* item;

    snprintf(buf, sizeof(buf), "%s", script);
    for (item = strtok_r(buf, ",", &save); item != NULL && nsteps < MAX_STEPS;
         item = strtok_r(NULL, ",", &save))
    {
        Step* s = &steps[nsteps];
        char* fields[3] = { item, NULL, NULL };
        char* save2 = NULL;
        char* name;

        memset(s, 0, sizeof(*s));
        s->lx = s->ly = s->rx = s->ry = 128;
        if ((fields[1] = strchr(item, ':')) == NULL) {
            ps3_log("pad_host: \"%s\" has no inputs\n", item);
            continue;
        }
        *fields[1]++ = '\0';
        if ((fields[2] = strchr(fields[1], ':')) != NULL) {
            *fields[2]++ = '\0';
        }
        s->at = (u32) strtoul(fields[0], NULL, 10);
        s->length = fields[2] != NULL ? (u32) strtoul(fields[2], NULL, 10) : 6;
        for (name = strtok_r(fields[1], "+", &save2); name != NULL; name = strtok_r(NULL, "+", &save2)) {
            if (!parse_input(name, s)) {
                ps3_log("pad_host: unknown input \"%s\"\n", name);
            }
        }
        nsteps++;
    }
    ps3_log("pad_host: %d scripted steps\n", nsteps);
}

void ps3_pad_init(void)
{
    const char* script = getenv("MELEE_HOST_PAD");

    if (script != NULL) {
        scripted = true;
        parse(script);
    }
}

void ps3_pad_read(Ps3Pad* pads, int n)
{
    u32 now = VIGetRetraceCount();
    int i;

    memset(pads, 0, sizeof(*pads) * (size_t) n);
    if (!scripted || n < 1) {
        return;
    }
    pads[0].connected = true;
    pads[0].lx = pads[0].ly = pads[0].rx = pads[0].ry = 128;
    for (i = 0; i < nsteps; i++) {
        const Step* s = &steps[i];

        if (now - s->at < s->length) {
            pads[0].buttons |= s->buttons;
            pads[0].l2 |= s->l2;
            pads[0].r2 |= s->r2;
            if (s->lx != 128) {
                pads[0].lx = s->lx;
            }
            if (s->ly != 128) {
                pads[0].ly = s->ly;
            }
            if (s->rx != 128) {
                pads[0].rx = s->rx;
            }
            if (s->ry != 128) {
                pads[0].ry = s->ry;
            }
        }
    }
    pads[0].cross = pads[0].buttons & PS3_PAD_CROSS ? 255 : 0;
    pads[0].square = pads[0].buttons & PS3_PAD_SQUARE ? 255 : 0;
    ps3_diag.pads = 1;
    ps3_diag.pad0_buttons = pads[0].buttons;
}

void ps3_pad_rumble(int index, bool on) {}
