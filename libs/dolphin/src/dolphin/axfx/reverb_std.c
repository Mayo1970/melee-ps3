#include <dolphin.h>

#include <dolphin/ax.h>
#include <dolphin/axfx.h>

// functions
static void DLsetdelay(struct AXFX_REVSTD_DELAYLINE* dl, s32 lag);
static void DLcreate(struct AXFX_REVSTD_DELAYLINE* dl, s32 max_length);
static void DLdelete(struct AXFX_REVSTD_DELAYLINE* dl);
static int ReverbSTDCreate(struct AXFX_REVSTD_WORK* rv, float coloration,
                           float time, float mix, float damping,
                           float predelay);
static int ReverbSTDModify(struct AXFX_REVSTD_WORK* rv, float coloration,
                           float time, float mix, float damping,
                           float predelay);
static void HandleReverb(s32* sptr, struct AXFX_REVSTD_WORK* rv);
static void ReverbSTDCallback(s32* left, s32* right, s32* surround,
                              struct AXFX_REVSTD_WORK* rv);
static void ReverbSTDFree(struct AXFX_REVSTD_WORK* rv);

static void DLsetdelay(struct AXFX_REVSTD_DELAYLINE* dl, s32 lag)
{
    dl->outPoint = dl->inPoint - (lag * 4);
    while (dl->outPoint < 0) {
        dl->outPoint += dl->length;
    }
}

static void DLcreate(struct AXFX_REVSTD_DELAYLINE* dl, s32 max_length)
{
    dl->length = (max_length * 4);
    dl->inputs = __AXFXAlloc(max_length * 4);
    memset(dl->inputs, 0, max_length * 4);
    dl->lastOutput = 0.0f;
    DLsetdelay(dl, max_length >> 1);
    dl->inPoint = 0;
    dl->outPoint = 0;
}

static void DLdelete(struct AXFX_REVSTD_DELAYLINE* dl)
{
    __AXFXFree(dl->inputs);
}

static int ReverbSTDCreate(struct AXFX_REVSTD_WORK* rv, float coloration,
                           float time, float mix, float damping,
                           float predelay)
{
    u8 i;
    u8 k;
    static s32 lens[4] = {
        0x000006FD,
        0x000007CF,
        0x000001B1,
        0x00000095,
    };

    if ((coloration < 0.0f) || (coloration > 1.0f) || (time < 0.01f) ||
        (time > 10.0f) || (mix < 0.0f) || (mix > 1.0f) || (damping < 0.0f) ||
        (damping > 1.0f) || (predelay < 0.0f) || (predelay > 0.1f))
    {
        return 0;
    }

    memset(rv, 0, sizeof(struct AXFX_REVSTD_WORK));
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 2; i++) {
            DLcreate(&rv->C[i + (k * 2)], lens[i] + 2);
            DLsetdelay(&rv->C[i + (k * 2)], lens[i]);
            rv->combCoef[i + (k * 2)] =
                powf(10.0f, (lens[i] * -3) / (32000.0f * time));
        }
        for (i = 0; i < 2; i++) {
            DLcreate(&rv->AP[i + (k * 2)], lens[i + 2] + 2);
            DLsetdelay(&rv->AP[i + (k * 2)], lens[i + 2]);
        }
        rv->lpLastout[k] = 0.0f;
    }
    rv->allPassCoeff = coloration;
    rv->level = mix;
    rv->damping = damping;
    if (rv->damping < 0.05f) {
        rv->damping = 0.05f;
    }
    rv->damping = (1.0f - (0.05f + (0.8f * rv->damping)));
    if (0.0f != predelay) {
        rv->preDelayTime = (32000.0f * predelay);
        for (i = 0; i < 3; i++) {
            rv->preDelayLine[i] = __AXFXAlloc(rv->preDelayTime * 4);
            memset(rv->preDelayLine[i], 0, rv->preDelayTime * 4);
            rv->preDelayPtr[i] = rv->preDelayLine[i];
        }
    } else {
        rv->preDelayTime = 0;
        for (i = 0; i < 3; i++) {
            rv->preDelayPtr[i] = 0;
            rv->preDelayLine[i] = 0;
        }
    }
    return 1;
}

static int ReverbSTDModify(struct AXFX_REVSTD_WORK* rv, float coloration,
                           float time, float mix, float damping,
                           float predelay)
{
    u8 i;

    if ((coloration < 0.0f) || (coloration > 1.0f) || (time < 0.01f) ||
        (time > 10.0f) || (mix < 0.0f) || (mix > 1.0f) || (damping < 0.0f) ||
        (damping > 1.0f) || (predelay < 0.0f) || (predelay > 100.0f))
    {
        return 0;
    }
    rv->allPassCoeff = coloration;
    rv->level = mix;
    rv->damping = damping;
    if (rv->damping < 0.05f) {
        rv->damping = 0.05f;
    }
    rv->damping = (1.0f - (0.05f + (0.8f * rv->damping)));
    for (i = 0; i < 6; i++) {
        DLdelete(&rv->AP[i]);
    }
    for (i = 0; i < 6; i++) {
        DLdelete(&rv->C[i]);
    }
    if (rv->preDelayTime) {
        for (i = 0; i < 3; i++) {
            __AXFXFree(rv->preDelayLine[i]);
        }
    }
    return ReverbSTDCreate(rv, coloration, time, mix, damping, predelay);
}

const static float value0_3 = 0.3f;
const static float value0_6 = 0.6f;
const static double i2fMagic = 4503601774854144.0;

#ifdef TARGET_PS3
// The C version of the assembly below. ReverbSTDCreate gives the three channels' delay lines the same lengths
// and points, and they move together: one loop runs the three chains, which the in-order PPE can overlap.
typedef struct RevStdChannel {
    u8* c0;
    u8* c1;
    u8* ap0;
    u8* ap1;
    u8* line;
    f32 c0Last, c1Last, ap0Last, ap1Last;
    f32 comb0, comb1, lp;
} RevStdChannel;

// The shared delay line points, as byte offsets.
typedef struct RevStdPoints {
    s64 c0In, c0Out, c1In, c1Out;
    s64 ap0In, ap0Out, ap1In, ap1Out;
} RevStdPoints;

// IVOPTS would turn the 8 shared points into 24 pointers. The loop runs each step below for the three channels
// before the next: GCC keeps the delay lines' accesses in order, so one chain would hold up the next channel's.
#define REVSTD_OPT AXFX_NO_CONTRACT __attribute__((optimize("no-ivopts")))
#define REVSTD_STEP static inline REVSTD_OPT __attribute__((always_inline))

REVSTD_STEP f32 RevStdPreDelay(u8* at, f32 x)
{
    f32 in = *(f32*) at;

    *(f32*) at = x;
    return in;
}

REVSTD_STEP void RevStdComb(RevStdChannel* ch, const RevStdPoints* p, f32 in)
{
    *(f32*) (ch->c0 + p->c0In) = AXFX_FMADDS(ch->comb0, ch->c0Last, in);
    *(f32*) (ch->c1 + p->c1In) = AXFX_FMADDS(ch->comb1, ch->c1Last, in);
    ch->c0Last = *(f32*) (ch->c0 + p->c0Out);
    ch->c1Last = *(f32*) (ch->c1 + p->c1Out);
}

REVSTD_STEP f32 RevStdAllPass0(RevStdChannel* ch, const RevStdPoints* p, f32 allPass)
{
    f32 v = AXFX_FMADDS(allPass, ch->ap0Last, ch->c0Last + ch->c1Last);
    f32 t;

    *(f32*) (ch->ap0 + p->ap0In) = v;
    t = AXFX_FNMSUBS(allPass, v, ch->ap0Last);
    ch->ap0Last = *(f32*) (ch->ap0 + p->ap0Out);
    return t;
}

REVSTD_STEP f32 RevStdAllPass1(RevStdChannel* ch, const RevStdPoints* p, f32 t, f32 allPass, f32 damping)
{
    f32 v;

    t = AXFX_FMADDS(damping, ch->lp, t * value0_3);
    ch->lp = t;
    v = AXFX_FMADDS(allPass, ch->ap1Last, t);
    *(f32*) (ch->ap1 + p->ap1In) = v;
    t = AXFX_FNMSUBS(allPass, v, ch->ap1Last);
    ch->ap1Last = *(f32*) (ch->ap1 + p->ap1Out);
    return t;
}

// __AXFXFctiwz into *out; a store in each branch lets the conversion go straight from the FP register.
REVSTD_STEP void RevStdOut(s32* out, f32 x)
{
    if (__builtin_expect(x >= 2147483648.0f, 0)) {
        *out = 0x7FFFFFFF;
    } else if (__builtin_expect(!(x >= -2147483648.0f), 0)) {
        *out = -0x7FFFFFFF - 1;
    } else {
        *out = (s32) x;
    }
}

// How many of n samples run before point reaches end and wraps; a point that never meets end leaves n.
REVSTD_STEP s64 RevStdRun(s64 n, s64 point, s64 end)
{
    s64 left = (end - point + 3) >> 2;

    return point < end && left < n ? left : n;
}

static void RevStdLoad(RevStdChannel* ch, struct AXFX_REVSTD_WORK* rv, s32 k)
{
    ch->c0 = (u8*) rv->C[k * 2].inputs;
    ch->c1 = (u8*) rv->C[k * 2 + 1].inputs;
    ch->ap0 = (u8*) rv->AP[k * 2].inputs;
    ch->ap1 = (u8*) rv->AP[k * 2 + 1].inputs;
    ch->line = (u8*) rv->preDelayLine[k];
    ch->c0Last = rv->C[k * 2].lastOutput;
    ch->c1Last = rv->C[k * 2 + 1].lastOutput;
    ch->ap0Last = rv->AP[k * 2].lastOutput;
    ch->ap1Last = rv->AP[k * 2 + 1].lastOutput;
    ch->comb0 = rv->combCoef[k * 2];
    ch->comb1 = rv->combCoef[k * 2 + 1];
    ch->lp = rv->lpLastout[k];
}

static void RevStdSave(const RevStdChannel* ch, const RevStdPoints* p, s64 pre, struct AXFX_REVSTD_WORK* rv,
                       s32 k)
{
    struct AXFX_REVSTD_DELAYLINE* c = &rv->C[k * 2];
    struct AXFX_REVSTD_DELAYLINE* ap = &rv->AP[k * 2];

    c[0].inPoint = p->c0In;
    c[0].outPoint = p->c0Out;
    c[1].inPoint = p->c1In;
    c[1].outPoint = p->c1Out;
    c[0].lastOutput = ch->c0Last;
    c[1].lastOutput = ch->c1Last;
    ap[0].inPoint = p->ap0In;
    ap[0].outPoint = p->ap0Out;
    ap[1].inPoint = p->ap1In;
    ap[1].outPoint = p->ap1Out;
    ap[0].lastOutput = ch->ap0Last;
    ap[1].lastOutput = ch->ap1Last;
    rv->lpLastout[k] = ch->lp;
    rv->preDelayPtr[k] = (f32*) (ch->line + pre);
}

// The three channels lie one after the other at sptr.
static REVSTD_OPT void HandleReverb(s32* sptr, struct AXFX_REVSTD_WORK* rv)
{
    // The samples reach the FP registers through memory, all stored before the first load: no load-hit-store.
    s64 xin[3 * 160];
    f32 allPass = rv->allPassCoeff;
    f32 damping = rv->damping;
    f32 wet = rv->level * value0_6;
    f32 dry = value0_6 - wet;
    s32 preDelay = rv->preDelayTime;
    // The pre-delay wraps one sample early, as in the assembly.
    s64 preEnd = (s64) (preDelay - 1) * 4;
    s64 pre = (u8*) rv->preDelayPtr[0] - (u8*) rv->preDelayLine[0];
    s64 c0Len = rv->C[0].length, c1Len = rv->C[1].length;
    s64 ap0Len = rv->AP[0].length, ap1Len = rv->AP[1].length;
    const s64* x = xin;
    s32* out = sptr;
    RevStdChannel ch0, ch1, ch2;
    RevStdPoints p;
    s64 i;
    s64 n;

    RevStdLoad(&ch0, rv, 0);
    RevStdLoad(&ch1, rv, 1);
    RevStdLoad(&ch2, rv, 2);
    p.c0In = rv->C[0].inPoint;
    p.c0Out = rv->C[0].outPoint;
    p.c1In = rv->C[1].inPoint;
    p.c1Out = rv->C[1].outPoint;
    p.ap0In = rv->AP[0].inPoint;
    p.ap0Out = rv->AP[0].outPoint;
    p.ap1In = rv->AP[1].inPoint;
    p.ap1Out = rv->AP[1].outPoint;
    for (i = 0; i < 3 * 160; i++) {
        xin[i] = sptr[i];
    }
    // Runs of samples between wraps, so the sample loop checks none.
    for (i = 0; i < 160; i += n) {
        s64 j;

        n = RevStdRun(160 - i, p.c0In, c0Len);
        n = RevStdRun(n, p.c0Out, c0Len);
        n = RevStdRun(n, p.c1In, c1Len);
        n = RevStdRun(n, p.c1Out, c1Len);
        n = RevStdRun(n, p.ap0In, ap0Len);
        n = RevStdRun(n, p.ap0Out, ap0Len);
        n = RevStdRun(n, p.ap1In, ap1Len);
        n = RevStdRun(n, p.ap1Out, ap1Len);
        if (preDelay != 0) {
            n = RevStdRun(n, pre, preEnd);
        }
        for (j = 0; j < n; j++) {
            f32 x0 = (f32) (f64) x[0];
            f32 x1 = (f32) (f64) x[160];
            f32 x2 = (f32) (f64) x[320];
            f32 in0 = x0, in1 = x1, in2 = x2;
            f32 t0, t1, t2;

            if (preDelay != 0) {
                in0 = RevStdPreDelay(ch0.line + pre, x0);
                in1 = RevStdPreDelay(ch1.line + pre, x1);
                in2 = RevStdPreDelay(ch2.line + pre, x2);
                pre += 4;
            }
            RevStdComb(&ch0, &p, in0);
            RevStdComb(&ch1, &p, in1);
            RevStdComb(&ch2, &p, in2);
            t0 = RevStdAllPass0(&ch0, &p, allPass);
            t1 = RevStdAllPass0(&ch1, &p, allPass);
            t2 = RevStdAllPass0(&ch2, &p, allPass);
            t0 = RevStdAllPass1(&ch0, &p, t0, allPass, damping);
            t1 = RevStdAllPass1(&ch1, &p, t1, allPass, damping);
            t2 = RevStdAllPass1(&ch2, &p, t2, allPass, damping);
            RevStdOut(&out[0], AXFX_FMADDS(wet, t0, dry * x0));
            RevStdOut(&out[160], AXFX_FMADDS(wet, t1, dry * x1));
            RevStdOut(&out[320], AXFX_FMADDS(wet, t2, dry * x2));
            p.c0In += 4;
            p.c0Out += 4;
            p.c1In += 4;
            p.c1Out += 4;
            p.ap0In += 4;
            p.ap0Out += 4;
            p.ap1In += 4;
            p.ap1Out += 4;
            x++;
            out++;
        }
        if (p.c0In == c0Len) {
            p.c0In = 0;
        }
        if (p.c0Out == c0Len) {
            p.c0Out = 0;
        }
        if (p.c1In == c1Len) {
            p.c1In = 0;
        }
        if (p.c1Out == c1Len) {
            p.c1Out = 0;
        }
        if (p.ap0In == ap0Len) {
            p.ap0In = 0;
        }
        if (p.ap0Out == ap0Len) {
            p.ap0Out = 0;
        }
        if (p.ap1In == ap1Len) {
            p.ap1In = 0;
        }
        if (p.ap1Out == ap1Len) {
            p.ap1Out = 0;
        }
        if (preDelay != 0 && pre == preEnd) {
            pre = 0;
        }
    }
    RevStdSave(&ch0, &p, pre, rv, 0);
    RevStdSave(&ch1, &p, pre, rv, 1);
    RevStdSave(&ch2, &p, pre, rv, 2);
}
#else
asm static void HandleReverb(register s32* sptr,
                             register struct AXFX_REVSTD_WORK* rv)
{
    // clang-format off
    nofralloc
	stwu r1, -144(r1)
	stmw r17, 8(r1)
	stfd f14, 88(r1)
	stfd f15, 96(r1)
	stfd f16, 104(r1)
	stfd f17, 112(r1)
	stfd f18, 120(r1)
	stfd f19, 128(r1)
	stfd f20, 136(r1)
	lis r31, value0_3@ha
	lfs f6, value0_3@l(r31)
	lis r31, value0_6@ha
	lfs f9, value0_6@l(r31)
	lis r31, i2fMagic@ha
	lfd f5, i2fMagic@l(r31)
	lfs f2, AXFX_REVSTD_WORK.allPassCoeff(rv)
	lfs f11, AXFX_REVSTD_WORK.damping(rv)
	lfs f8, AXFX_REVSTD_WORK.level(rv)
	fmuls f3, f8, f9
	fsubs f4, f9, f3
	lis r30, 0x4330 // 176.0f (0x43300000)
	stw r30, 80(r1)
	li r5, 0
L_00000638:
	slwi r31, r5, 3
	add r31, r31, rv
	lfs f19, AXFX_REVSTD_WORK.combCoef[0](r31)
	lfs f20, AXFX_REVSTD_WORK.combCoef[1](r31)
	slwi r31, r5, 2
	add r31, r31, rv
	lfs f7, AXFX_REVSTD_WORK.lpLastout[0](r31)
	lwz r27, AXFX_REVSTD_WORK.preDelayLine[0](r31)
	lwz r28, AXFX_REVSTD_WORK.preDelayPtr[0](r31)
	lwz r31, AXFX_REVSTD_WORK.preDelayTime(rv)
	subi r22, r31, 1
	slwi r22, r22, 2
	add r22, r22, r27
	cmpwi cr7, r31, 0
	mulli r31, r5, 0x28 // sizeof(struct AXFX_REVSTD_DELAYLINE * 2)
	addi r29, rv, AXFX_REVSTD_WORK.C
	add r29, r29, r31
	addi r30, rv, AXFX_REVSTD_WORK.AP
	add r30, r30, r31
	lwz r21, AXFX_REVSTD_DELAYLINE.inPoint    + 0x00(r29) // C array + 0
	lwz r20, AXFX_REVSTD_DELAYLINE.outPoint   + 0x00(r29) // C array + 0
	lwz r19, AXFX_REVSTD_DELAYLINE.inPoint    + 0x14(r29) // C array + 1
	lwz r18, AXFX_REVSTD_DELAYLINE.outPoint   + 0x14(r29) // C array + 1
	lfs f15, AXFX_REVSTD_DELAYLINE.lastOutput + 0x00(r29) // C array + 0
	lfs f16, AXFX_REVSTD_DELAYLINE.lastOutput + 0x14(r29) // C array + 1
	lwz r26, AXFX_REVSTD_DELAYLINE.length     + 0x00(r29) // C array + 0
	lwz r25, AXFX_REVSTD_DELAYLINE.length     + 0x14(r29) // C array + 1
	lwz r7,  AXFX_REVSTD_DELAYLINE.inputs     + 0x00(r29) // C array + 0
	lwz r8,  AXFX_REVSTD_DELAYLINE.inputs     + 0x14(r29) // C array + 1
	lwz r12, AXFX_REVSTD_DELAYLINE.inPoint    + 0x00(r30) // AP array + 0
	lwz r11, AXFX_REVSTD_DELAYLINE.outPoint   + 0x00(r30) // AP array + 0
	lwz r10, AXFX_REVSTD_DELAYLINE.inPoint    + 0x14(r30) // AP array + 1
	lwz r9,  AXFX_REVSTD_DELAYLINE.outPoint   + 0x14(r30) // AP array + 1
	lfs f17, AXFX_REVSTD_DELAYLINE.lastOutput + 0x00(r30) // AP array + 0
	lfs f18, AXFX_REVSTD_DELAYLINE.lastOutput + 0x14(r30) // AP array + 1
	lwz r24, AXFX_REVSTD_DELAYLINE.length     + 0x00(r30) // AP array + 0
	lwz r23, AXFX_REVSTD_DELAYLINE.length     + 0x14(r30) // AP array + 1
	lwz r17, AXFX_REVSTD_DELAYLINE.inputs     + 0x00(r30) // AP array + 0
	lwz r6,  AXFX_REVSTD_DELAYLINE.inputs     + 0x14(r30) // AP array + 1
	lwz r30, 0(sptr)
	xoris r30, r30, 0x8000
	stw r30, 84(r1)
	lfd f12, 80(r1)
	fsubs f12, f12, f5
	li r31, 159
	mtctr r31
L_000006F0:
	fmr f13, f12
	beq cr7, L_00000710
	lfs f13, 0(r28)
	addi r28, r28, 4
	cmpw r28, r22
	stfs f12, -4(r28)
	bne+ L_00000710
	mr r28, r27
L_00000710:
	fmadds f8, f19, f15, f13
	lwzu r29, 4(sptr)
	fmadds f9, f20, f16, f13
	stfsx f8, r7, r21
	addi r21, r21, 4
	stfsx f9, r8, r19
	lfsx f14, r7, r20
	addi r20, r20, 4
	lfsx f16, r8, r18
	cmpw r21, r26
	cmpw cr1, r20, r26
	addi r19, r19, 4
	addi r18, r18, 4
	fmr f15, f14
	cmpw cr5, r19, r25
	fadds f14, f14, f16
	cmpw cr6, r18, r25
	bne+ L_0000075C
	li r21, 0
L_0000075C:
	xoris r29, r29, 0x8000
	fmadds f9, f2, f17, f14
	bne+ cr1, L_0000076C
	li r20, 0
L_0000076C:
	stw r29, 84(r1)
	bne+ cr5, L_00000778
	li r19, 0
L_00000778:
	stfsx f9, r17, r12
	fnmsubs f14, f2, f9, f17
	addi r12, r12, 4
	bne+ cr6, L_0000078C
	li r18, 0
L_0000078C:
	lfsx f17, r17, r11
	cmpw cr5, r12, r24
	addi r11, r11, 4
	cmpw cr6, r11, r24
	bne+ cr5, L_000007A4
	li r12, 0
L_000007A4:
	bne+ cr6, L_000007AC
	li r11, 0
L_000007AC:
	fmuls f14, f14, f6
	lfd f10, 80(r1)
	fmadds f14, f11, f7, f14
	fmadds f9, f2, f18, f14
	fmr f7, f14
	stfsx f9, r6, r10
	fnmsubs f14, f2, f9, f18
	fmuls f8, f4, f12
	lfsx f18, r6, r9
	addi r10, r10, 4
	addi r9, r9, 4
	fmadds f14, f3, f14, f8
	cmpw cr5, r10, r23
	cmpw cr6, r9, r23
	fctiwz f14, f14
	bne+ cr5, L_000007F0
	li r10, 0
L_000007F0:
	bne+ cr6, L_000007F8
	li r9, 0
L_000007F8:
	li r31, -4
	fsubs f12, f10, f5
	stfiwx f14, sptr, r31
	bdnz L_000006F0
	fmr f13, f12
	beq cr7, L_00000828
	lfs f13, 0(r28)
	addi r28, r28, 4
	cmpw r28, r22
	stfs f12, -4(r28)
	bne+ L_00000828
	mr r28, r27
L_00000828:
	fmadds f8, f19, f15, f13
	fmadds f9, f20, f16, f13
	stfsx f8, r7, r21
	addi r21, r21, 4
	stfsx f9, r8, r19
	lfsx f14, r7, r20
	addi r20, r20, 4
	lfsx f16, r8, r18
	cmpw r21, r26
	cmpw cr1, r20, r26
	addi r19, r19, 4
	addi r18, r18, 4
	fmr f15, f14
	cmpw cr5, r19, r25
	fadds f14, f14, f16
	cmpw cr6, r18, r25
	bne+ L_00000870
	li r21, 0
L_00000870:
	fmadds f9, f2, f17, f14
	bne+ cr1, L_0000087C
	li r20, 0
L_0000087C:
	bne+ cr5, L_00000884
	li r19, 0
L_00000884:
	stfsx f9, r17, r12
	fnmsubs f14, f2, f9, f17
	addi r12, r12, 4
	bne+ cr6, L_00000898
	li r18, 0
L_00000898:
	lfsx f17, r17, r11
	cmpw cr5, r12, r24
	addi r11, r11, 4
	cmpw cr6, r11, r24
	bne+ cr5, L_000008B0
	li r12, 0
L_000008B0:
	bne+ cr6, L_000008B8
	li r11, 0
L_000008B8:
	fmuls f14, f14, f6
	fmadds f14, f11, f7, f14
	mulli r31, r5, 0x28 // sizeof(struct AXFX_REVSTD_DELAYLINE * 2)
	fmadds f9, f2, f18, f14
	fmr f7, f14
	addi r29, rv, AXFX_REVSTD_WORK.C
	add r29, r29, r31
	stfsx f9, r6, r10
	fnmsubs f14, f2, f9, f18
	fmuls f8, f4, f12
	lfsx f18, r6, r9
	addi r10, r10, 4
	addi r9, r9, 4
	fmadds f14, f3, f14, f8
	cmpw cr5, r10, r23
	cmpw cr6, r9, r23
	fctiwz f14, f14
	bne+ cr5, L_00000904
	li r10, 0
L_00000904:
	bne+ cr6, L_0000090C
	li r9, 0
L_0000090C:
	addi r30, rv, AXFX_REVSTD_WORK.AP
	add r30, r30, r31
	stfiwx f14, r0, sptr
	stw r21, AXFX_REVSTD_DELAYLINE.inPoint  + 0x00(r29) // C array + 0
	stw r20, AXFX_REVSTD_DELAYLINE.outPoint + 0x00(r29) // C array + 0
	stw r19, AXFX_REVSTD_DELAYLINE.inPoint  + 0x14(r29) // C array + 1
	stw r18, AXFX_REVSTD_DELAYLINE.outPoint + 0x14(r29) // C array + 1
	addi sptr, sptr, 4
	stfs f15, AXFX_REVSTD_DELAYLINE.lastOutput + 0x00(r29) // C array + 0
	stfs f16, AXFX_REVSTD_DELAYLINE.lastOutput + 0x14(r29) // C array + 1
	slwi r31, r5, 2
	add r31, r31, rv
	addi r5, r5, 1
	stw r12, AXFX_REVSTD_DELAYLINE.inPoint  + 0x00(r30) // AP array + 0
	stw r11, AXFX_REVSTD_DELAYLINE.outPoint + 0x00(r30) // AP array + 0
	stw r10, AXFX_REVSTD_DELAYLINE.inPoint  + 0x14(r30) // AP array + 1
	stw r9, AXFX_REVSTD_DELAYLINE.outPoint  + 0x14(r30) // AP array + 1
	cmpwi r5, 3
	stfs f17, AXFX_REVSTD_DELAYLINE.lastOutput + 0x00(r30) // AP array + 0
	stfs f18, AXFX_REVSTD_DELAYLINE.lastOutput + 0x14(r30) // AP array + 1
	stfs f7, AXFX_REVSTD_WORK.lpLastout(r31)
	stw r28, AXFX_REVSTD_WORK.preDelayPtr(r31)
	bne L_00000638
	lfd f14, 88(r1)
	lfd f15, 96(r1)
	lfd f16, 104(r1)
	lfd f17, 112(r1)
	lfd f18, 120(r1)
	lfd f19, 128(r1)
	lfd f20, 136(r1)
	lmw r17, 8(r1)
	addi r1, r1, 144
	blr
    // clang-format on
}
#endif

static void ReverbSTDCallback(s32* left, s32* right, s32* surround,
                              struct AXFX_REVSTD_WORK* rv)
{
    HandleReverb(left, rv);
}

static void ReverbSTDFree(struct AXFX_REVSTD_WORK* rv)
{
    u8 i;

    for (i = 0; i < 6; i++) {
        DLdelete(&rv->AP[i]);
    }
    for (i = 0; i < 6; i++) {
        DLdelete(&rv->C[i]);
    }
    if (rv->preDelayTime) {
        for (i = 0; i < 3; i++) {
            __AXFXFree(rv->preDelayLine[i]);
        }
    }
}

int AXFXReverbStdInit(struct AXFX_REVERBSTD* rev)
{
    int ret;
    int old;

    old = OSDisableInterrupts();
    rev->tempDisableFX = 0;
    ret = ReverbSTDCreate(&rev->rv, rev->coloration, rev->time, rev->mix,
                          rev->damping, rev->preDelay);
    OSRestoreInterrupts(old);
    return ret;
}

int AXFXReverbStdShutdown(struct AXFX_REVERBSTD* rev)
{
    int old;

    old = OSDisableInterrupts();
    ReverbSTDFree(&rev->rv);
    OSRestoreInterrupts(old);
    return 1;
}

int AXFXReverbStdSettings(struct AXFX_REVERBSTD* rev)
{
    int old;

    old = OSDisableInterrupts();
    rev->tempDisableFX = 1;
    ReverbSTDModify(&rev->rv, rev->coloration, rev->time, rev->mix,
                    rev->damping, rev->preDelay);
    rev->tempDisableFX = 0;
    OSRestoreInterrupts(old);
    return 1;
}

void AXFXReverbStdCallback(struct AXFX_BUFFERUPDATE* bufferUpdate,
                           struct AXFX_REVERBSTD* reverb)
{
    if (reverb->tempDisableFX == 0) {
        ReverbSTDCallback(bufferUpdate->left, bufferUpdate->right,
                          bufferUpdate->surround, &reverb->rv);
    }
}
