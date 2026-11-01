#include "fobj.h"

#include <string.h>

#include "debug.h"
#include "spline.h"

HSD_ObjAllocData fobj_alloc_data;

#ifdef TARGET_PS3
// fterm_f lives in what was tail padding (LP64), so HSD_FObj keeps its size and every offset.
_Static_assert(sizeof(HSD_FObj) == 64, "fterm_f must fit the tail padding");
#endif

HSD_ObjAllocData* HSD_FObjGetAllocData(void)
{
    return &fobj_alloc_data;
}

void HSD_FObjInitAllocData(void)
{
    HSD_ObjAllocInit(HSD_FObjGetAllocData(), sizeof(HSD_FObj), 4);
}

void HSD_FObjRemove(HSD_FObj* fobj)
{
    if (!fobj) {
        return;
    }

    HSD_FObjFree(fobj);
}

void HSD_FObjRemoveAll(HSD_FObj* fobj)
{
    if (fobj == NULL) {
        return;
    }
    HSD_FObjRemoveAll(fobj->next);
    HSD_FObjRemove(fobj);
}

u32 HSD_FObjSetState(HSD_FObj* fobj, u32 state)
{
    if (fobj) {
        fobj->flags = (state & 0xF) | (fobj->flags & 0xF0);
    }
    return state;
}

u32 HSD_FObjGetState(HSD_FObj* fobj)
{
    if (!fobj) {
        return 0;
    }
    return fobj->flags & 0xF;
}

static inline void HSD_FObjReqAnim(HSD_FObj* fobj, f32 startframe)
{
    if (fobj == NULL) {
        return;
    }

    fobj->ad = fobj->ad_head;
    fobj->time = (f32) fobj->startframe + startframe;
    fobj->op = 0;
    fobj->op_intrp = 0;
    fobj->flags &= ~0x40;
    fobj->nb_pack = 0;
    fobj->fterm = 0;
#ifdef TARGET_PS3
    fobj->fterm_f = 0.f;
#endif
    fobj->p0 = 0.f;
    fobj->p1 = 0.f;
    fobj->d0 = 0.f;
    fobj->d1 = 0.f;
    HSD_FObjSetState(fobj, 1);
}

void HSD_FObjReqAnimAll(HSD_FObj* fobj, f32 startframe)
{
    HSD_FObj* fp;

    if (fobj == NULL) {
        return;
    }

    for (fp = fobj; fp != NULL; fp = fp->next) {
        HSD_FObjReqAnim(fp, startframe);
    }
}

static inline void FObj_FlushKeyData(HSD_FObj* fobj, void* obj,
                                     HSD_ObjUpdateFunc obj_update, f32 rate)
{
    if (fobj->op_intrp == HSD_A_OP_KEY) {
        HSD_FObjInterpretAnim(fobj, obj, obj_update, rate);
    }
}

void HSD_FObjStopAnim(HSD_FObj* fobj, void* obj, HSD_ObjUpdateFunc obj_update,
                      f32 rate)
{
    if (fobj == NULL) {
        return;
    }

    FObj_FlushKeyData(fobj, obj, obj_update, rate);
    HSD_FObjSetState(fobj, 0);
}

void HSD_FObjStopAnimAll(HSD_FObj* fobj, void* obj,
                         HSD_ObjUpdateFunc obj_update, f32 rate)
{
    for (; fobj != NULL; fobj = fobj->next) {
        HSD_FObjStopAnim(fobj, obj, obj_update, rate);
    }
}

union parseFloat_u {
    f32 f;
    u32 d;
};

#ifdef TARGET_PS3
#define FOBJ_T4(n) n, n + 1, n + 2, n + 3
#define FOBJ_T16(n) FOBJ_T4(n), FOBJ_T4(n + 4), FOBJ_T4(n + 8), FOBJ_T4(n + 12)
#define FOBJ_T64(n) FOBJ_T16(n), FOBJ_T16(n + 16), FOBJ_T16(n + 32), FOBJ_T16(n + 48)
static const f32 parseFloat_u8[256] = { FOBJ_T64(0), FOBJ_T64(64), FOBJ_T64(128), FOBJ_T64(192) };
static const f32 parseFloat_s8[256] = { FOBJ_T64(0), FOBJ_T64(64), FOBJ_T64(-128), FOBJ_T64(-64) };
// 1 / (1 << n) as an s32: 1 << 31 is INT_MIN, so the last is -2^-31.
static const f32 parseFloat_recip[32] = {
    0x1p0f,   0x1p-1f,  0x1p-2f,  0x1p-3f,  0x1p-4f,  0x1p-5f,  0x1p-6f,  0x1p-7f,  0x1p-8f,  0x1p-9f,   0x1p-10f,
    0x1p-11f, 0x1p-12f, 0x1p-13f, 0x1p-14f, 0x1p-15f, 0x1p-16f, 0x1p-17f, 0x1p-18f, 0x1p-19f, 0x1p-20f,  0x1p-21f,
    0x1p-22f, 0x1p-23f, 0x1p-24f, 0x1p-25f, 0x1p-26f, 0x1p-27f, 0x1p-28f, 0x1p-29f, 0x1p-30f, -0x1p-31f,
};
#undef FOBJ_T64
#undef FOBJ_T16
#undef FOBJ_T4

// The integers come from tables and scale by an exact power of two: the same bits as numer / denom, without the
// integer to float conversions through memory and the divide.
static f32 parseFloat(u8** pos, u8 frac)
{
    union parseFloat_u u;
    u8* p = *pos;
    f32 numer;

    if (frac == HSD_A_FRAC_FLOAT) {
        u.d = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
        *pos = p + 4;
        return u.f;
    }

    switch (frac & 0xE0) {
    case HSD_A_FRAC_S8:
        numer = parseFloat_s8[p[0]];
        *pos = p + 1;
        break;
    case HSD_A_FRAC_U8:
        numer = parseFloat_u8[p[0]];
        *pos = p + 1;
        break;
    case HSD_A_FRAC_S16:
        numer = parseFloat_s8[p[1]] * 256.0f + parseFloat_u8[p[0]];
        *pos = p + 2;
        break;
    case HSD_A_FRAC_U16:
        numer = parseFloat_u8[p[1]] * 256.0f + parseFloat_u8[p[0]];
        *pos = p + 2;
        break;
    default:
        return 0.0f;
    }
    return numer * parseFloat_recip[frac & 0x1F];
}
#else
static f32 parseFloat(u8** pos, u8 frac)
{
    union parseFloat_u u;
    f32 numer;
    s32 denom;

    if (frac == HSD_A_FRAC_FLOAT) {
        u.d = (s32) ((*pos)++)[0];
        u.d |= ((*pos)++)[0] << 8;
        u.d |= ((*pos)++)[0] << 16;
        u.d |= ((*pos)++)[0] << 24;
        return u.f;
    }

    denom = (1 << (frac & 0x1F));
    switch (frac & 0xE0) {
    case HSD_A_FRAC_S8:
        numer = (s8) (*pos)[0];
        *pos += 1;
        break;
    case HSD_A_FRAC_U8:
        numer = (*pos)[0];
        *pos += 1;
        break;
    case HSD_A_FRAC_S16:
        numer = ((s8) (*pos)[1] << 8) | (*pos)[0];
        *pos += 2;
        break;
    case HSD_A_FRAC_U16:
        numer = ((*pos)[1] << 8) | (*pos)[0];
        *pos += 2;
        break;
    default:
        return 0.0f;
    }
    return numer / denom;
}
#endif

static u8 parseOpCode(u8** curr_parse)
{
    return (**curr_parse) & 0xF;
}

static u32 parsePackInfo(u8** adp)
{
    u8 d;
    u32 nb_pack;
    s32 shift;

    d = *(*adp)++;
    nb_pack = ((d >> 4) & 7) + 1;
    shift = 3;
    if (!(d & 0x80)) {
        return nb_pack;
    }
    do {
        d = *(*adp)++;
        nb_pack += (d & 0x7F) << shift;
        shift += 7;
    } while (d & 0x80);
    return nb_pack;
}

static void FObjLaunchKeyData(HSD_FObj* fobj)
{
    if ((fobj->flags & 0x40) != 0) {
        fobj->op_intrp = fobj->op;
        fobj->flags &= ~0x40;
        fobj->flags |= 0x80;
        fobj->p0 = fobj->p1;
    }
}

static s32 parseWait(u8** adp)
{
    u8 d;
    s32 wait = 0;
    s32 shift = 0;

    do {
        d = *(*adp)++;
        wait |= (d & 0x7f) << shift;
        shift += 7;
    } while (d & 0x80);

    return wait;
}

static u32 FObjLoadWait(HSD_FObj* fobj)
{
    u32 st = HSD_FObjGetState(fobj);
    HSD_ASSERT(0x16C, st == FOBJ_LOAD_WAIT);

    if ((unsigned) (fobj->ad - fobj->ad_head) >= fobj->length) {
        return 6;
    } else {
        fobj->fterm = parseWait(&fobj->ad);
#ifdef TARGET_PS3
        fobj->fterm_f = fobj->fterm;
#endif
        fobj->flags |= 0x20;
        return HSD_FObjSetState(fobj, 2);
    }
}

static u32 FObjAnimCON(HSD_FObj* fobj)
{
    u32 st = HSD_FObjGetState(fobj);
    HSD_ASSERT(0x17F, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    fobj->p0 = fobj->p1;
    fobj->p1 = parseFloat(&fobj->ad, fobj->frac_value);
    if (fobj->op_intrp != 5) {
        fobj->d0 = fobj->d1;
        fobj->d1 = 0.0F;
    }

    return HSD_FObjSetState(fobj, st == FOBJ_LOAD_DATA0 ? 3 : 4);
}

static u32 FObjAnimLinear(HSD_FObj* fobj)
{
    u32 st = HSD_FObjGetState(fobj);
    HSD_ASSERT(0x193, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    fobj->p0 = fobj->p1;
    fobj->p1 = parseFloat(&fobj->ad, fobj->frac_value);
    if (fobj->op_intrp != 5) {
        fobj->d0 = fobj->d1;
        fobj->d1 = 0.0F;
    }

    return HSD_FObjSetState(fobj, st == FOBJ_LOAD_DATA0 ? 3 : 4);
}

static u32 FObjAnimSPL0(HSD_FObj* fobj)
{
    u32 st = HSD_FObjGetState(fobj);
    HSD_ASSERT(0x1A7, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    fobj->p0 = fobj->p1;
    fobj->d0 = fobj->d1;
    fobj->p1 = parseFloat(&fobj->ad, fobj->frac_value);
    fobj->d1 = 0.0F;

    return HSD_FObjSetState(fobj, st == FOBJ_LOAD_DATA0 ? 3 : 4);
}

static u32 FObjAnimSPL(HSD_FObj* fobj)
{
    u32 st = HSD_FObjGetState(fobj);
    HSD_ASSERT(0x1B9, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    fobj->p0 = fobj->p1;
    fobj->p1 = parseFloat(&fobj->ad, fobj->frac_value);
    fobj->d0 = fobj->d1;
    fobj->d1 = parseFloat(&fobj->ad, fobj->frac_slope);

    return HSD_FObjSetState(fobj, st == FOBJ_LOAD_DATA0 ? 3 : 4);
}

static u32 FObjAnimSLP(HSD_FObj* fobj)
{
    u32 st = HSD_FObjGetState(fobj);
    HSD_ASSERT(0x1CC, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    fobj->d0 = fobj->d1;
    fobj->d1 = parseFloat(&fobj->ad, fobj->frac_slope);

    return HSD_FObjGetState(fobj);
}

static u32 FObjAnimKey(HSD_FObj* fobj)
{
    u32 st = HSD_FObjGetState(fobj);
    HSD_ASSERT(0x1E9, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    FObjLaunchKeyData(fobj);
    fobj->p1 = parseFloat(&fobj->ad, fobj->frac_value);
    fobj->flags |= 0x40;

    return HSD_FObjSetState(fobj, st == FOBJ_LOAD_DATA0 ? 3 : 4);
}

static inline u32 FObjLoadData(HSD_FObj* fobj)
{
    if ((unsigned) (fobj->ad - fobj->ad_head) >= fobj->length) {
        return 6;
    } else {
        fobj->op_intrp = fobj->op;
        if (fobj->nb_pack == 0) {
            fobj->op = parseOpCode(&fobj->ad);
            fobj->nb_pack = parsePackInfo(&fobj->ad);
        }

        fobj->nb_pack -= 1;

        switch (fobj->op) {
        case HSD_A_OP_CON:
            return FObjAnimCON(fobj);

        case HSD_A_OP_LIN:
            return FObjAnimLinear(fobj);

        case HSD_A_OP_SPL0:
            return FObjAnimSPL0(fobj);

        case HSD_A_OP_SPL:
            return FObjAnimSPL(fobj);

        case HSD_A_OP_SLP:
            return FObjAnimSLP(fobj);

        case HSD_A_OP_KEY:
            return FObjAnimKey(fobj);

        default:
            return 0;
        }
    }
}

void FObjUpdateAnim(HSD_FObj* fobj, void* obj, HSD_ObjUpdateFunc obj_update)
{
    f32 phi_f0;
    HSD_ObjData fobjdata;

    if (obj_update == NULL) {
        return;
    }
    switch (fobj->op_intrp) {
    case HSD_A_OP_KEY:
        if (fobj->flags & 0x80) {
            fobjdata.fv = fobj->p0;
            fobj->flags &= 0xFFFFFF7F;
        } else {
            return;
        }
        break;
    case HSD_A_OP_CON:
#ifdef TARGET_PS3
        if (fobj->time >= fobj->fterm_f) {
#else
        if (fobj->time >= fobj->fterm) {
#endif
            phi_f0 = fobj->p1;
        } else {
            phi_f0 = fobj->p0;
        }
        fobjdata.fv = phi_f0;
        break;
    case HSD_A_OP_LIN:
        if (fobj->flags & 0x20) {
            fobj->flags = fobj->flags & 0xFFFFFFDF;
            if (fobj->fterm != 0) {
#ifdef TARGET_PS3
                fobj->d0 = (fobj->p1 - fobj->p0) / fobj->fterm_f;
#else
                fobj->d0 = (fobj->p1 - fobj->p0) / fobj->fterm;
#endif
            } else {
                fobj->d0 = 0;
                fobj->p0 = fobj->p1;
            }
        }
        fobjdata.fv = fobj->d0 * fobj->time + fobj->p0;
        break;
    case HSD_A_OP_SPL0:
    case HSD_A_OP_SPL:
    case HSD_A_OP_SLP:
        if (fobj->fterm != 0) {
#ifdef TARGET_PS3
            // (f32) (1.0 / fterm) as one fdivs: a quotient rounded to double, then to float, is still correctly
            // rounded (53 >= 2 * 24 + 2 bits).
            fobjdata.fv = splGetHelmite(1.0f / fobj->fterm_f, fobj->time, fobj->p0, fobj->p1, fobj->d0, fobj->d1);
#else
            fobjdata.fv =
                splGetHelmite(1.0 / fobj->fterm, fobj->time, fobj->p0,
                              fobj->p1, fobj->d0, fobj->d1);
#endif
        } else {
            fobjdata.fv = fobj->p1;
        }
        break;
    default:
        break;
    }
    obj_update(obj, fobj->obj_type, &fobjdata);
}

void HSD_FObjInterpretAnim(HSD_FObj* fobj, void* obj,
                           HSD_ObjUpdateFunc obj_update, f32 rate)
{
    f32 fterm;
    u32 state;

    fterm = 0.0F;
    state = fobj != NULL ? HSD_FObjGetState(fobj) : 0;
    if (state != 0 && !(fobj->time += rate, (fobj->time < 0.0))) {
        for (;;) {
            switch (state) {
            case 6: {
                fobj->time += fterm;
                FObjLaunchKeyData(fobj);
                FObjUpdateAnim(fobj, obj, obj_update);
                return;
            }
            case 1:
            case 2: {
                state = FObjLoadData(fobj);
                break;
            }
            case 3: {
                if ((fobj->flags & 0x80) != 0) {
                    FObjUpdateAnim(fobj, obj, obj_update);
                }
                state = FObjLoadWait(fobj);
                break;
            }
            case 4: {
#ifdef TARGET_PS3
                if (fobj->fterm_f <= fobj->time) {
#else
                if (fobj->fterm <= fobj->time) {
#endif
                    u8 _[8];
                    state =
#ifdef MUST_MATCH
                        state =
#endif
                            3;

#ifdef TARGET_PS3
                    fterm = fobj->fterm_f;
                    fobj->time -= fobj->fterm_f;
#else
                    fterm = fobj->fterm;
                    fobj->time -= fobj->fterm;
#endif
                    HSD_FObjSetState(fobj, state);
                    break;
                }
                FObjUpdateAnim(fobj, obj, obj_update);
                state =
#ifdef MUST_MATCH
                    state =
#endif
                        5;
                HSD_FObjSetState(fobj, state);
                return;
            }
            case 5: {
                state =
#ifdef MUST_MATCH
                    state =
#endif
                        4;
                HSD_FObjSetState(fobj, state);
                break;
            }
            case 0:
                return;
            }
        }
    }
}

void HSD_FObjInterpretAnimAll(void* fobj, void* obj,
                              HSD_ObjUpdateFunc obj_update, f32 rate)
{
    HSD_FObj* fobjNew = (HSD_FObj*) fobj;
    while (fobjNew != NULL) {
        HSD_FObjInterpretAnim(fobjNew, obj, obj_update, rate);
        fobjNew = fobjNew->next;
    }
}

HSD_FObj* HSD_FObjLoadDesc(HSD_FObjDesc* desc)
{
    if (desc != NULL) {
        HSD_FObj* fobj = HSD_FObjAlloc();
        fobj->next = HSD_FObjLoadDesc(desc->next);
        fobj->startframe = desc->startframe;
        fobj->obj_type = desc->type;
        fobj->frac_value = desc->frac_value;
        fobj->frac_slope = desc->frac_slope;
        fobj->ad_head = desc->ad;
        fobj->length = desc->length;
        fobj->flags = 0;
        return fobj;
    }
    return NULL;
}

HSD_FObj* HSD_FObjAlloc(void)
{
    HSD_FObj* new = HSD_ObjAlloc(HSD_FObjGetAllocData());
    HSD_ASSERT(0x2F3, new);
    memset(new, 0, sizeof(HSD_FObj));
    return new;
}

void HSD_FObjFree(HSD_FObj* fobj)
{
    HSD_ObjFree(HSD_FObjGetAllocData(), fobj);
}
