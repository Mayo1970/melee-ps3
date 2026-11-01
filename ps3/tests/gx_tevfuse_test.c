// Host test (qemu-ppc64): GXSetTevStagePS3 against the seven calls HSD_SetupTevStage made before, on random stages
// and shadows. The SDK state must match whole, and so must the last value written to each BP register.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/gx.h>
#include <dolphin/os.h>

#include "../../libs/dolphin/src/dolphin/gx/__gx.h"

#define ROUNDS 2000000
#define PIPE_SIZE 0x1000

struct __GXData_struct __GXData;

u8* __GXPipeWp;
u8* __GXPipeLimit;
static u8 pipe[PIPE_SIZE + GX_PIPE_CMD_MAX];
static u32 seed = 0x12345678;
static int failures;

void __GXPipeMakeRoom(void)
{
    printf("FAIL: pipe full\n");
    exit(1);
}

void __GXPipeFlush(void) {}

void OSReport(char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
    printf("OSPanic %s:%d\n", file, line);
    exit(1);
}

static u32 rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

// Mostly in range, sometimes past the field into the next ones, rarely any 32-bit value.
static u32 rval(u32 range)
{
    u32 r = rnd() % 64;

    if (r == 0) {
        return rnd();
    }
    if (r < 4) {
        return rnd() % (range * 64);
    }
    return rnd() % range;
}

typedef struct Result {
    struct __GXData_struct state;
    u8 stream[PIPE_SIZE];
    u32 size;
} Result;

static void run(const struct __GXData_struct* start, int fused, u32 stage, const GXTevStagePS3* c, Result* r)
{
    memcpy(&__GXData, start, sizeof(__GXData));
    __GXPipeWp = pipe;
    __GXPipeLimit = pipe + PIPE_SIZE;
    if (fused) {
        GXSetTevStagePS3(stage, c);
    } else {
        GXSetTevColorOp(stage, c->clr_op, c->clr_bias, c->clr_scale, c->clr_clamp, c->clr_out_reg);
        GXSetTevColorIn(stage, c->clr_a, c->clr_b, c->clr_c, c->clr_d);
        GXSetTevAlphaOp(stage, c->alpha_op, c->alpha_bias, c->alpha_scale, c->alpha_clamp, c->alpha_out_reg);
        GXSetTevAlphaIn(stage, c->alpha_a, c->alpha_b, c->alpha_c, c->alpha_d);
        GXSetTevSwapMode(stage, c->ras_swap, c->tex_swap);
        GXSetTevKColorSel(stage, c->kcsel);
        GXSetTevKAlphaSel(stage, c->kasel);
    }
    memcpy(&r->state, &__GXData, sizeof(__GXData));
    r->size = (u32) (__GXPipeWp - pipe);
    memcpy(r->stream, pipe, r->size);
}

// The last value written to each BP register, and which were written; 0 if the stream is not all BP writes.
static int last_values(const Result* r, u32 last[256], u8 written[256])
{
    u32 i;

    memset(written, 0, 256);
    for (i = 0; i + 5 <= r->size; i += 5) {
        u32 v = (u32) r->stream[i + 1] << 24 | (u32) r->stream[i + 2] << 16 | (u32) r->stream[i + 3] << 8 |
                r->stream[i + 4];

        if (r->stream[i] != 0x61) {
            return 0;
        }
        last[v >> 24] = v & 0xFFFFFF;
        written[v >> 24] = 1;
    }
    return i == r->size;
}

static void fail(u32 round, const char* what)
{
    if (failures++ < 20) {
        printf("FAIL round %u: %s\n", round, what);
    }
}

int main(void)
{
    static struct __GXData_struct start;
    static Result old, fused;
    u32 old_last[256], fused_last[256];
    u8 old_written[256], fused_written[256];
    u32 round, spilled = 0, writes_old = 0, writes_fused = 0;
    int i;

    for (round = 0; round < ROUNDS; round++) {
        GXTevStagePS3 c;
        u32 stage = rnd() % 64 == 0 ? 16 + rnd() % 4 : rnd() % 16;
        u8* p = (u8*) &start;

        for (i = 0; i < (int) sizeof(start); i++) {
            p[i] = (u8) rnd();
        }
        // The register numbers GXInit puts in the top bytes, as a rule.
        for (i = 0; i < 16; i++) {
            if (rnd() % 16 != 0) {
                start.tevc[i] = (start.tevc[i] & 0xFFFFFF) | (0xC0 + i * 2) << 24;
                start.teva[i] = (start.teva[i] & 0xFFFFFF) | (0xC1 + i * 2) << 24;
            }
        }
        for (i = 0; i < 8; i++) {
            if (rnd() % 16 != 0) {
                start.tevKsel[i] = (start.tevKsel[i] & 0xFFFFFF) | (0xF6 + i) << 24;
            }
        }
        memset(&c, 0, sizeof(c));
        c.clr_op = rval(16);
        c.clr_a = rval(16);
        c.clr_b = rval(16);
        c.clr_c = rval(16);
        c.clr_d = rval(16);
        c.clr_scale = rval(4);
        c.clr_bias = rval(4);
        c.clr_clamp = (u8) rval(2);
        c.clr_out_reg = rval(4);
        c.alpha_op = rval(16);
        c.alpha_a = rval(8);
        c.alpha_b = rval(8);
        c.alpha_c = rval(8);
        c.alpha_d = rval(8);
        c.alpha_scale = rval(4);
        c.alpha_bias = rval(4);
        c.alpha_clamp = (u8) rval(2);
        c.alpha_out_reg = rval(4);
        c.ras_swap = rval(4);
        c.tex_swap = rval(4);
        c.kcsel = rval(32);
        c.kasel = rval(32);

        run(&start, 0, stage, &c, &old);
        run(&start, 1, stage, &c, &fused);
        writes_old += old.size / 5;
        writes_fused += fused.size / 5;
        if (memcmp(&old.state, &fused.state, sizeof(old.state)) != 0) {
            fail(round, "SDK state differs");
        }
        if (!last_values(&old, old_last, old_written) || !last_values(&fused, fused_last, fused_written)) {
            fail(round, "stream is not BP writes");
            continue;
        }
        if (fused.size == 15) {
            // The fused path: the same registers, with the same last values.
            for (i = 0; i < 256; i++) {
                if (old_written[i] != fused_written[i] || (old_written[i] && old_last[i] != fused_last[i])) {
                    fail(round, "BP register differs");
                    break;
                }
            }
        } else if (old.size != fused.size || memcmp(old.stream, fused.stream, old.size) != 0) {
            // The calls' path: the same stream.
            fail(round, "stream differs");
        } else {
            spilled++;
        }
    }
    printf("%u rounds, %u through the calls; BP writes %u before, %u fused\n", ROUNDS, spilled, writes_old,
           writes_fused);
    printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
