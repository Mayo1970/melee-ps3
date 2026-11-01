// Host test of reverb_std.c's HandleReverb against the per-channel C version it replaced: both run from copies
// of one state on the same aux frames, and every output sample and the whole reverb state must match bit for bit.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../libs/dolphin/src/dolphin/axfx/reverb_std.c"

#define GUARD 64
#define GUARD_BYTE 0xA5
#define MAX_BLOCKS 64
#define FRAMES 1500

// The function as it was, one channel after the other.
static AXFX_NO_CONTRACT void HandleReverbOld(s32* sptr, struct AXFX_REVSTD_WORK* rv)
{
    f32 allPass = rv->allPassCoeff;
    f32 damping = rv->damping;
    f32 wet = rv->level * value0_6;
    f32 dry = value0_6 - wet;
    s32 k;

    for (k = 0; k < 3; k++) {
        struct AXFX_REVSTD_DELAYLINE* c = &rv->C[k * 2];
        struct AXFX_REVSTD_DELAYLINE* ap = &rv->AP[k * 2];
        u8* c0 = (u8*) c[0].inputs;
        u8* c1 = (u8*) c[1].inputs;
        u8* ap0 = (u8*) ap[0].inputs;
        u8* ap1 = (u8*) ap[1].inputs;
        s32 c0In = c[0].inPoint, c0Out = c[0].outPoint;
        s32 c1In = c[1].inPoint, c1Out = c[1].outPoint;
        s32 ap0In = ap[0].inPoint, ap0Out = ap[0].outPoint;
        s32 ap1In = ap[1].inPoint, ap1Out = ap[1].outPoint;
        f32 c0Last = c[0].lastOutput, c1Last = c[1].lastOutput;
        f32 ap0Last = ap[0].lastOutput, ap1Last = ap[1].lastOutput;
        f32 comb0 = rv->combCoef[k * 2];
        f32 comb1 = rv->combCoef[k * 2 + 1];
        f32 lp = rv->lpLastout[k];
        f32* line = rv->preDelayLine[k];
        f32* ptr = rv->preDelayPtr[k];
        int i;

        for (i = 0; i < 160; i++) {
            f32 x = (f32) sptr[i];
            f32 in = x;
            f32 t;
            f32 v;

            if (rv->preDelayTime != 0) {
                in = *ptr;
                *ptr++ = x;
                if (ptr == line + (rv->preDelayTime - 1)) {
                    ptr = line;
                }
            }
            *(f32*) (c0 + c0In) = AXFX_FMADDS(comb0, c0Last, in);
            *(f32*) (c1 + c1In) = AXFX_FMADDS(comb1, c1Last, in);
            c0Last = *(f32*) (c0 + c0Out);
            c1Last = *(f32*) (c1 + c1Out);
            if ((c0In += 4) == c[0].length) {
                c0In = 0;
            }
            if ((c0Out += 4) == c[0].length) {
                c0Out = 0;
            }
            if ((c1In += 4) == c[1].length) {
                c1In = 0;
            }
            if ((c1Out += 4) == c[1].length) {
                c1Out = 0;
            }
            t = c0Last + c1Last;

            v = AXFX_FMADDS(allPass, ap0Last, t);
            *(f32*) (ap0 + ap0In) = v;
            t = AXFX_FNMSUBS(allPass, v, ap0Last);
            ap0Last = *(f32*) (ap0 + ap0Out);
            if ((ap0In += 4) == ap[0].length) {
                ap0In = 0;
            }
            if ((ap0Out += 4) == ap[0].length) {
                ap0Out = 0;
            }

            t = AXFX_FMADDS(damping, lp, t * value0_3);
            lp = t;
            v = AXFX_FMADDS(allPass, ap1Last, t);
            *(f32*) (ap1 + ap1In) = v;
            t = AXFX_FNMSUBS(allPass, v, ap1Last);
            ap1Last = *(f32*) (ap1 + ap1Out);
            if ((ap1In += 4) == ap[1].length) {
                ap1In = 0;
            }
            if ((ap1Out += 4) == ap[1].length) {
                ap1Out = 0;
            }

            sptr[i] = __AXFXFctiwz(AXFX_FMADDS(wet, t, dry * x));
        }
        c[0].inPoint = c0In;
        c[0].outPoint = c0Out;
        c[1].inPoint = c1In;
        c[1].outPoint = c1Out;
        c[0].lastOutput = c0Last;
        c[1].lastOutput = c1Last;
        ap[0].inPoint = ap0In;
        ap[0].outPoint = ap0Out;
        ap[1].inPoint = ap1In;
        ap[1].outPoint = ap1Out;
        ap[0].lastOutput = ap0Last;
        ap[1].lastOutput = ap1Last;
        rv->lpLastout[k] = lp;
        rv->preDelayPtr[k] = ptr;
        sptr += 160;
    }
}

// Every block has guard bytes on both sides, to catch a write outside a delay line.
static struct {
    u8* base;
    unsigned long size;
} blocks[MAX_BLOCKS];
static int nblocks;

static void* guarded_alloc(unsigned long size)
{
    u8* p = malloc(size + 2 * GUARD);

    if (p == NULL || nblocks == MAX_BLOCKS) {
        fprintf(stderr, "allocation failed\n");
        exit(2);
    }
    memset(p, GUARD_BYTE, size + 2 * GUARD);
    blocks[nblocks].base = p;
    blocks[nblocks].size = size;
    nblocks++;
    return p + GUARD;
}

static int guards_broken;

static int block_intact(int i)
{
    int j;

    for (j = 0; j < GUARD; j++) {
        if (blocks[i].base[j] != GUARD_BYTE || blocks[i].base[GUARD + blocks[i].size + j] != GUARD_BYTE) {
            return 0;
        }
    }
    return 1;
}

static void check_guards(void)
{
    int i;

    for (i = 0; i < nblocks; i++) {
        guards_broken += !block_intact(i);
    }
}

static void guarded_free(void* ptr)
{
    int i;

    for (i = 0; i < nblocks; i++) {
        if (blocks[i].base + GUARD == ptr) {
            guards_broken += !block_intact(i);
            free(blocks[i].base);
            blocks[i] = blocks[--nblocks];
            return;
        }
    }
    fprintf(stderr, "free of an unknown block\n");
    exit(2);
}

void* (*__AXFXAlloc)(unsigned long) = guarded_alloc;
void (*__AXFXFree)(void*) = guarded_free;

BOOL OSDisableInterrupts(void)
{
    return FALSE;
}

BOOL OSRestoreInterrupts(BOOL level)
{
    return level;
}

static void* dup_block(const void* src, unsigned long size)
{
    void* p = guarded_alloc(size);

    memcpy(p, src, size);
    return p;
}

// A deep copy, so the two functions work on equal but separate delay lines.
static void clone_state(struct AXFX_REVSTD_WORK* dst, const struct AXFX_REVSTD_WORK* src)
{
    int i;

    *dst = *src;
    for (i = 0; i < 6; i++) {
        dst->C[i].inputs = dup_block(src->C[i].inputs, src->C[i].length);
        dst->AP[i].inputs = dup_block(src->AP[i].inputs, src->AP[i].length);
    }
    for (i = 0; i < 3; i++) {
        if (src->preDelayTime != 0) {
            dst->preDelayLine[i] = dup_block(src->preDelayLine[i], src->preDelayTime * 4);
            dst->preDelayPtr[i] = dst->preDelayLine[i] + (src->preDelayPtr[i] - src->preDelayLine[i]);
        }
    }
}

// Bit for bit, except that two NaNs match: which NaN operand an FMA passes on depends on the order GCC gives the
// two factors, and no output can show it (every NaN clamps to INT_MIN).
static int nan_payloads;

static int same_floats(const f32* a, const f32* b, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (memcmp(&a[i], &b[i], sizeof(f32)) != 0) {
            if (a[i] == a[i] || b[i] == b[i]) {
                return 0;
            }
            nan_payloads++;
        }
    }
    return 1;
}

static int same_line(const struct AXFX_REVSTD_DELAYLINE* a, const struct AXFX_REVSTD_DELAYLINE* b)
{
    return a->inPoint == b->inPoint && a->outPoint == b->outPoint && a->length == b->length &&
           same_floats(&a->lastOutput, &b->lastOutput, 1) && same_floats(a->inputs, b->inputs, a->length / 4);
}

static int same_state(const struct AXFX_REVSTD_WORK* a, const struct AXFX_REVSTD_WORK* b)
{
    int i;

    for (i = 0; i < 6; i++) {
        if (!same_line(&a->C[i], &b->C[i]) || !same_line(&a->AP[i], &b->AP[i])) {
            return 0;
        }
    }
    if (!same_floats(a->lpLastout, b->lpLastout, 3) || a->preDelayTime != b->preDelayTime) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        if (a->preDelayTime != 0 &&
            (a->preDelayPtr[i] - a->preDelayLine[i] != b->preDelayPtr[i] - b->preDelayLine[i] ||
             !same_floats(a->preDelayLine[i], b->preDelayLine[i], a->preDelayTime)))
        {
            return 0;
        }
    }
    return 1;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static u32 next_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (u32) (rng >> 16);
}

// Silence, small and 24-bit noise, any 32-bit value, sparse impulses, a ramp, and the extremes.
static void make_frame(s32* buf, int frame)
{
    int mode = (frame / 40) % 7;
    int i;

    for (i = 0; i < 3 * 160; i++) {
        s32 v = 0;

        switch (mode) {
        case 1:
            v = (s32) (next_random() % 65536) - 32768;
            break;
        case 2:
            v = (s32) (next_random() % 16777216) - 8388608;
            break;
        case 3:
            v = (s32) next_random();
            break;
        case 4:
            v = next_random() % 97 == 0 ? (s32) (next_random() % 16777216) - 8388608 : 0;
            break;
        case 5:
            v = (s32) ((i % 160) * 4099 * (i / 160 + 1)) % 1000000 - 500000;
            break;
        case 6:
            v = (next_random() & 1) ? 0x7FFFFFFF : -0x7FFFFFFF - 1;
            break;
        }
        buf[i] = v;
    }
}

typedef struct Config {
    float coloration, time, mix, damping, predelay;
} Config;

// Melee's default (64 samples of pre-delay), none, the longest, a 2-sample one that wraps every sample, and others.
static const Config configs[] = {
    { 0.5f, 1.48f, 1.0f, 0.64f, 0.002f }, { 0.5f, 1.48f, 1.0f, 0.64f, 0.0f },   { 0.9f, 10.0f, 0.3f, 0.1f, 0.1f },
    { 0.0f, 0.01f, 0.5f, 1.0f, 0.05f },   { 1.0f, 3.0f, 0.0f, 0.0f, 0.00007f }, { 0.3f, 0.7f, 0.8f, 0.02f, 0.0031f },
};

int main(void)
{
    static s32 in_old[3 * 160];
    static s32 in_new[3 * 160];
    long long samples = 0;
    long long saturated_hi = 0;
    long long saturated_lo = 0;
    int failures = 0;
    int c;

    for (c = 0; c < (int) (sizeof(configs) / sizeof(configs[0])); c++) {
        const Config* cf = &configs[c];
        struct AXFX_REVSTD_WORK old_rv;
        struct AXFX_REVSTD_WORK new_rv;
        int bad_frames = 0;
        int frame;

        if (!ReverbSTDCreate(&old_rv, cf->coloration, cf->time, cf->mix, cf->damping, cf->predelay)) {
            printf("config %d: ReverbSTDCreate failed  FAIL\n", c);
            failures++;
            continue;
        }
        clone_state(&new_rv, &old_rv);
        for (frame = 0; frame < FRAMES; frame++) {
            make_frame(in_old, frame);
            // Infinities and NaNs in the state reach the outputs' clamps; the state is made again later.
            if (frame % 500 == 100 || frame % 500 == 300) {
                f32 poison = frame % 500 == 100 ? __builtin_inff() : __builtin_nanf("");
                int k = frame / 500 % 3;

                old_rv.C[k * 2].inputs[frame % 7] = poison;
                new_rv.C[k * 2].inputs[frame % 7] = poison;
                old_rv.AP[k * 2 + 1].inputs[3] = -poison;
                new_rv.AP[k * 2 + 1].inputs[3] = -poison;
            }
            if (frame % 500 == 200 || frame % 500 == 400) {
                ReverbSTDModify(&old_rv, cf->coloration, cf->time, cf->mix, cf->damping, cf->predelay);
                ReverbSTDFree(&new_rv);
                clone_state(&new_rv, &old_rv);
            }
            memcpy(in_new, in_old, sizeof(in_old));
            HandleReverbOld(in_old, &old_rv);
            HandleReverb(in_new, &new_rv);
            check_guards();
            samples += 3 * 160;
            {
                int i;

                for (i = 0; i < 3 * 160; i++) {
                    saturated_hi += in_old[i] == 0x7FFFFFFF;
                    saturated_lo += in_old[i] == -0x7FFFFFFF - 1;
                }
            }
            if (memcmp(in_old, in_new, sizeof(in_old)) != 0 || !same_state(&old_rv, &new_rv)) {
                if (bad_frames == 0) {
                    printf("config %d: frame %d differs (out %d, state %d)\n", c, frame,
                           memcmp(in_old, in_new, sizeof(in_old)) != 0, !same_state(&old_rv, &new_rv));
                }
                bad_frames++;
                ReverbSTDFree(&new_rv);
                clone_state(&new_rv, &old_rv);
            }
        }
        printf("config %d (pre-delay %d samples): %d frames, %d differ%s\n", c, (int) old_rv.preDelayTime, FRAMES,
               bad_frames, bad_frames != 0 ? "  FAIL" : "");
        failures += bad_frames != 0;
        ReverbSTDFree(&old_rv);
        ReverbSTDFree(&new_rv);
    }
    if (guards_broken != 0) {
        printf("%d writes outside a delay line  FAIL\n", guards_broken);
        failures++;
    }
    printf("%lld samples compared, %lld saturated high, %lld low or NaN; %d state words with another NaN\n", samples,
           saturated_hi, saturated_lo, nan_payloads);
    failures += saturated_hi == 0 || saturated_lo == 0;
    printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures != 0;
}
