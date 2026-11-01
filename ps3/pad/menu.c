// The PS3 menu: the pad thread runs it, the GP thread draws its picture with HSD's debug font when it changed,
// and the presents show that picture over the game (gx_rsx_set_screen).
#include "menu.h"

#include <stdio.h>
#include <string.h>

#include <dolphin/os.h>
#include <ps3_options.h>
#include <sysdolphin/baselib/hsd_3915.h>

#include "../gx/backend.h"

#ifndef PS3_OPTIONS_PATH
#define PS3_OPTIONS_PATH "/dev_hdd0/game/MELE00001/USRDIR/options.txt"
#endif

// The rows are a multiple of 16 pixels (64 bytes), as the RSX's linear textures want.
#define IMG_W 400
#define IMG_H 184
#define CELL_W 10
#define LINE_H 18
#define MARGIN 12
#define LABEL_W 130
#define COL_W 60
#define STICK_DIR 48

#define COLOR_BORDER 0xFF6878A0u
#define COLOR_PANEL 0xD0101828u
#define COLOR_TITLE 0xFFFFD040u
#define COLOR_TEXT 0xFFFFFFFFu
#define COLOR_DIM 0xFFA0A8B8u
#define COLOR_CURSOR 0xFF3050A0u

enum { ROW_WIDESCREEN, ROW_TAP_JUMP, ROW_CSTICK, ROWS };
enum { DIR_UP = 1, DIR_DOWN = 2, DIR_LEFT = 4, DIR_RIGHT = 8 };

Ps3Options ps3_options = { false, { true, true, true, true }, { false, false, false, false } };

static volatile bool open, in_match;
static volatile int row, col;
// Changes with the picture; the GP thread draws it again when it differs from drawn.
static volatile u32 serial = 1;
static u32 drawn;
static u32 image[IMG_W * IMG_H] __attribute__((aligned(128)));
static u16 held;
static u8 dirs;
static bool swallow, changed;

static void load_flags(bool* v, const char* s)
{
    int i;

    for (i = 0; i < 4 && (s[i] == '0' || s[i] == '1'); i++) {
        v[i] = s[i] == '1';
    }
}

static void load(void)
{
    FILE* f = fopen(PS3_OPTIONS_PATH, "r");
    char line[64], v[8];

    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "widescreen=%1s", v) == 1) {
            ps3_options.widescreen = v[0] == '1';
        } else if (sscanf(line, "tap_jump=%4s", v) == 1) {
            load_flags(ps3_options.tap_jump, v);
        } else if (sscanf(line, "cstick_tilts=%4s", v) == 1) {
            load_flags(ps3_options.cstick_tilts, v);
        }
    }
    fclose(f);
}

static void save_flags(FILE* f, const char* name, const bool* v)
{
    fprintf(f, "%s=%c%c%c%c\n", name, v[0] ? '1' : '0', v[1] ? '1' : '0', v[2] ? '1' : '0', v[3] ? '1' : '0');
}

static void save(void)
{
    FILE* f = fopen(PS3_OPTIONS_PATH, "w");

    if (f == NULL) {
        OSReport("menu: cannot write %s\n", PS3_OPTIONS_PATH);
        return;
    }
    fprintf(f, "widescreen=%c\n", ps3_options.widescreen ? '1' : '0');
    save_flags(f, "tap_jump", ps3_options.tap_jump);
    save_flags(f, "cstick_tilts", ps3_options.cstick_tilts);
    fclose(f);
}

static void fill(int x, int y, int w, int h, u32 argb)
{
    int i, j;

    for (j = y; j < y + h; j++) {
        for (i = x; i < x + w; i++) {
            image[j * IMG_W + i] = argb;
        }
    }
}

// A glyph is 14 big-endian words of 16 2-bit pixels: 1 the outline, 2 a shade, 3 the colour.
static void text(int x, int y, const char* s, u32 argb)
{
    u32 shade = 0xFF000000u | ((argb >> 1) & 0x7F7F7Fu);

    for (; *s != '\0'; s++, x += CELL_W) {
        const u8* g = HSD_DebugFontAtlas[(u8) *s & 0x7F].data;
        int r, i;

        for (r = 0; r < 14; r++) {
            u32 w = (u32) g[r * 4] << 24 | (u32) g[r * 4 + 1] << 16 | (u32) g[r * 4 + 2] << 8 | g[r * 4 + 3];

            for (i = 0; i < 12 && x + i < IMG_W; i++) {
                u32 v = (w >> ((15 - i) * 2)) & 3;

                if (v != 0) {
                    image[(y + r) * IMG_W + x + i] = v == 1 ? 0xFF000000u : v == 2 ? shade : argb;
                }
            }
        }
    }
}

static void draw(void)
{
    static const char* const labels[ROWS] = { "WIDESCREEN", "TAP JUMP", "C-STICK" };
    static const int lines[ROWS] = { 2, 5, 6 };
    char port[3] = "P1";
    int r, c;

    fill(0, 0, IMG_W, IMG_H, COLOR_BORDER);
    fill(2, 2, IMG_W - 4, IMG_H - 4, COLOR_PANEL);
    text((IMG_W - 11 * CELL_W) / 2, MARGIN, "PS3 OPTIONS", COLOR_TITLE);
    for (c = 0; c < 4; c++) {
        port[1] = (char) ('1' + c);
        text(MARGIN + LABEL_W + c * COL_W + 4, MARGIN + 4 * LINE_H, port, COLOR_DIM);
    }
    for (r = 0; r < ROWS; r++) {
        int y = MARGIN + lines[r] * LINE_H;

        text(MARGIN, y, labels[r], COLOR_TEXT);
        for (c = 0; c < (r == ROW_WIDESCREEN ? 1 : 4); c++) {
            int x = MARGIN + LABEL_W + c * COL_W;
            const char* v = r == ROW_WIDESCREEN ? (ps3_options.widescreen ? "ON" : "OFF")
                            : r == ROW_TAP_JUMP ? (ps3_options.tap_jump[c] ? "ON" : "OFF")
                                                : (ps3_options.cstick_tilts[c] ? "TILT" : "SMASH");

            if (r == row && (r == ROW_WIDESCREEN || c == col)) {
                fill(x, y - 2, COL_W - 4, LINE_H, COLOR_CURSOR);
            }
            text(x + 4, y, v, COLOR_TEXT);
        }
    }
    text(MARGIN, MARGIN + 8 * LINE_H, "A CHANGE   B CLOSE", COLOR_DIM);
}

// The GP thread, at each present. A change during the draw leaves serial ahead of drawn: the next one draws again.
static void screen(GxRsxScreen* out)
{
    u32 s = serial;

    out->wide = ps3_options.widescreen;
    out->overlay.pixels = NULL;
    if (!open || in_match) {
        return;
    }
    if (s != drawn) {
        draw();
        drawn = s;
    }
    out->overlay.pixels = image;
    out->overlay.w = IMG_W;
    out->overlay.h = IMG_H;
    out->overlay.serial = drawn;
}

void ps3_menu_init(void)
{
    load();
    gx_rsx_set_screen(screen);
}

void ps3_menu_match(bool on)
{
    in_match = on;
}

static void close_menu(void)
{
    open = false;
    swallow = true;
    if (changed) {
        changed = false;
        save();
    }
}

static void toggle(void)
{
    bool* v = row == ROW_WIDESCREEN ? &ps3_options.widescreen
              : row == ROW_TAP_JUMP ? &ps3_options.tap_jump[col]
                                    : &ps3_options.cstick_tilts[col];

    *v = !*v;
    changed = true;
}

static void navigate(u16 down, u8 dir)
{
    if (dir & DIR_UP) {
        row = (row + ROWS - 1) % ROWS;
    } else if (dir & DIR_DOWN) {
        row = (row + 1) % ROWS;
    } else if (dir & (DIR_LEFT | DIR_RIGHT)) {
        if (row == ROW_WIDESCREEN) {
            toggle();
        } else {
            col = (col + ((dir & DIR_LEFT) ? 3 : 1)) % 4;
        }
    } else if (down & PAD_BUTTON_A) {
        toggle();
    } else {
        return;
    }
    serial++;
}

static u8 stick_dirs(const PADStatus* s)
{
    u8 d = 0;

    d |= ((s->button & PAD_BUTTON_UP) || s->stickY > STICK_DIR) ? DIR_UP : 0;
    d |= ((s->button & PAD_BUTTON_DOWN) || s->stickY < -STICK_DIR) ? DIR_DOWN : 0;
    d |= ((s->button & PAD_BUTTON_LEFT) || s->stickX < -STICK_DIR) ? DIR_LEFT : 0;
    d |= ((s->button & PAD_BUTTON_RIGHT) || s->stickX > STICK_DIR) ? DIR_RIGHT : 0;
    return d;
}

static void rest(PADStatus status[PAD_MAX_CONTROLLERS])
{
    int c;

    for (c = 0; c < PAD_MAX_CONTROLLERS; c++) {
        s8 err = status[c].err;

        memset(&status[c], 0, sizeof(status[c]));
        status[c].err = err;
    }
}

void ps3_menu_pad(PADStatus status[PAD_MAX_CONTROLLERS], bool select)
{
    u16 now = 0, down;
    u8 dir = 0, dir_down;
    int c;

    for (c = 0; c < PAD_MAX_CONTROLLERS; c++) {
        if (status[c].err == PAD_ERR_NONE) {
            now |= status[c].button;
            dir |= stick_dirs(&status[c]);
        }
    }
    down = now & ~held;
    dir_down = dir & ~dirs;
    held = now;
    dirs = dir;
    // GameCube pads have no Select.
    if ((now & PAD_TRIGGER_Z) && (down & PAD_BUTTON_UP)) {
        select = true;
    }
    // A demo match can start under the open menu.
    if (open && in_match) {
        close_menu();
    }
    if (!open) {
        if (!select || in_match) {
            // The buttons that closed the menu reach the game only after they are up.
            if (swallow && now != 0) {
                for (c = 0; c < PAD_MAX_CONTROLLERS; c++) {
                    status[c].button = 0;
                }
            } else {
                swallow = false;
            }
            return;
        }
        open = true;
        row = col = 0;
        serial++;
    } else if (select || (down & (PAD_BUTTON_B | PAD_BUTTON_START))) {
        close_menu();
    } else {
        navigate(down, dir_down);
    }
    rest(status);
}
