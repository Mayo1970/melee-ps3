// RSX vertex and fragment ucode interpreters. See rsx_interp.h.
#include "rsx_interp.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

static void swizzle(const float* v, u32 sw, int neg, int abs_, float out[4])
{
    int i;

    for (i = 0; i < 4; i++) {
        float x = v[(sw >> (6 - 2 * i)) & 3];

        if (abs_) {
            x = fabsf(x);
        }
        out[i] = neg ? -x : x;
    }
}

static void vector_op(int op, const float* a, const float* b, const float* c, float r[4])
{
    int i;

    switch (op) {
    case 0x01: memcpy(r, a, 16); break;
    case 0x02: for (i = 0; i < 4; i++) r[i] = a[i] * b[i]; break;
    case 0x03: for (i = 0; i < 4; i++) r[i] = a[i] + c[i]; break;
    case 0x04: for (i = 0; i < 4; i++) r[i] = a[i] * b[i] + c[i]; break;
    case 0x05: r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
    case 0x07: r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; break;
    case 0x09: for (i = 0; i < 4; i++) r[i] = a[i] < b[i] ? a[i] : b[i]; break;
    case 0x0A: for (i = 0; i < 4; i++) r[i] = a[i] > b[i] ? a[i] : b[i]; break;
    case 0x0B: for (i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
    case 0x0C: for (i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
    case 0x0D:
    case 0x0F: for (i = 0; i < 4; i++) r[i] = floorf(a[i]); break;
    case 0x0E: for (i = 0; i < 4; i++) r[i] = a[i] - floorf(a[i]); break;
    default: memset(r, 0, 16); break;
    }
}

static float scalar_op(int op, float x)
{
    switch (op) {
    case 0x01: return x;
    case 0x02: return 1.0f / x;
    case 0x04: return 1.0f / sqrtf(x);
    case 0x0D: return log2f(x);
    case 0x0E: return exp2f(x);
    default: return 0.0f;
    }
}

static void write_masked(float* d, const float* r, u32 nvmask)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (nvmask & (8u >> i)) {
            d[i] = r[i];
        }
    }
}

void vp_run(const u32 (*w)[4], int n, const float (*c)[4], const float in[16][4], float out[16][4])
{
    float t[32][4], a0[4] = { 0, 0, 0, 0 };
    int k;

    memset(t, 0, sizeof(t));
    for (k = 0; k < n; k++) {
        const u32* hw = w[k];
        u32 src[3];
        float v[3][4], r[4];
        int vop = (hw[1] >> 22) & 0x1F, sop = (hw[1] >> 27) & 0x1F, i;
        int cidx = (hw[1] >> 12) & 0x3FF, input = (hw[1] >> 8) & 0xF;
        bool sat = (hw[0] >> 26) & 1;

        src[0] = ((hw[1] & 0xFF) << 9) | (hw[2] >> 23);
        src[1] = (hw[2] >> 6) & 0x1FFFF;
        src[2] = ((hw[2] & 0x3F) << 11) | (hw[3] >> 21);
        if (hw[3] & 2) {
            cidx += (int) a0[hw[0] & 3];
        }
        for (i = 0; i < 3; i++) {
            const float* reg;

            switch (src[i] & 3) {
            case 1: reg = t[(src[i] >> 2) & 0x1F]; break;
            case 2: reg = in[input]; break;
            default: reg = c[cidx]; break;
            }
            swizzle(reg, (src[i] >> 8) & 0xFF, (src[i] >> 16) & 1, (hw[0] >> (21 + i)) & 1, v[i]);
        }
        if (vop != 0) {
            u32 mask = (hw[3] >> 13) & 0xF;

            vector_op(vop, v[0], v[1], v[2], r);
            if (sat) {
                for (i = 0; i < 4; i++) r[i] = r[i] < 0 ? 0 : r[i] > 1 ? 1 : r[i];
            }
            if (vop == 0x0D) {
                write_masked(a0, r, mask);
            } else if (hw[0] & (1u << 30)) {
                write_masked(out[(hw[3] >> 2) & 0x1F], r, mask);
            } else if (((hw[0] >> 15) & 0x3F) != 0x3F) {
                write_masked(t[(hw[0] >> 15) & 0x3F], r, mask);
            }
        }
        if (sop != 0) {
            u32 mask = (hw[3] >> 17) & 0xF;
            float x = scalar_op(sop, v[2][0]);

            if (sat) {
                x = x < 0 ? 0 : x > 1 ? 1 : x;
            }
            r[0] = r[1] = r[2] = r[3] = x;
            if (hw[3] & (1u << 12)) {
                write_masked(out[(hw[3] >> 2) & 0x1F], r, mask);
            } else if (((hw[3] >> 7) & 0x3F) != 0x3F) {
                write_masked(t[(hw[3] >> 7) & 0x3F], r, mask);
            }
        }
        if (hw[3] & 1) {
            break;
        }
    }
}

static u32 unswap(u32 v)
{
    return v << 16 | v >> 16;
}

static void fp_vector(int op, const float* a, const float* b, const float* c, float r[4])
{
    int i;

    switch (op) {
    case 0x01: memcpy(r, a, 16); break;
    case 0x02: for (i = 0; i < 4; i++) r[i] = a[i] * b[i]; break;
    case 0x03: for (i = 0; i < 4; i++) r[i] = a[i] + b[i]; break;
    case 0x04: for (i = 0; i < 4; i++) r[i] = a[i] * b[i] + c[i]; break;
    case 0x05: r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
    case 0x06: r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; break;
    case 0x08: for (i = 0; i < 4; i++) r[i] = a[i] < b[i] ? a[i] : b[i]; break;
    case 0x09: for (i = 0; i < 4; i++) r[i] = a[i] > b[i] ? a[i] : b[i]; break;
    case 0x0A: for (i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
    case 0x0B: for (i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
    case 0x10: for (i = 0; i < 4; i++) r[i] = a[i] - floorf(a[i]); break;
    case 0x11: for (i = 0; i < 4; i++) r[i] = floorf(a[i]); break;
    case 0x1A: r[0] = r[1] = r[2] = r[3] = 1.0f / a[0]; break;
    case 0x1C: r[0] = r[1] = r[2] = r[3] = exp2f(a[0]); break;
    case 0x1D: r[0] = r[1] = r[2] = r[3] = log2f(a[0]); break;
    default: memset(r, 0, 16); break;
    }
}

void fp_run(const u32* ucode, int words, const float in[16][4], FpSampler sample, void* user, float out[4],
            float* depth)
{
    float t[64][4];
    int k = 0;

    memset(t, 0, sizeof(t));
    while (k + 4 <= words) {
        u32 w[4], src[3];
        float v[3][4], r[4], konst[4] = { 0, 0, 0, 0 };
        int op, i, dst, input;
        bool has_const = false;

        for (i = 0; i < 4; i++) {
            w[i] = unswap(ucode[k + i]);
        }
        k += 4;
        op = (w[0] >> 24) & 0x3F;
        dst = (w[0] >> 1) & 0x3F;
        input = (w[0] >> 13) & 0xF;
        for (i = 0; i < 3; i++) {
            src[i] = w[1 + i];
            has_const |= (src[i] & 3) == 2;
        }
        if (has_const) {
            for (i = 0; i < 4; i++) {
                u32 b = unswap(ucode[k + i]);
                memcpy(&konst[i], &b, 4);
            }
            k += 4;
        }
        for (i = 0; i < 3; i++) {
            const float* reg;
            u32 sw = (src[i] >> 9) & 0xFF;
            int j;

            switch (src[i] & 3) {
            case 0: reg = t[(src[i] >> 2) & 0x3F]; break;
            case 1: reg = in[input]; break;
            default: reg = konst; break;
            }
            for (j = 0; j < 4; j++) {
                float x = reg[(sw >> (2 * j)) & 3];
                v[i][j] = (src[i] >> 17) & 1 ? -x : x;
            }
        }
        if (op == 0x17 || op == 0x18) {
            float coord[4];

            memcpy(coord, v[0], 16);
            if (op == 0x18) {
                coord[0] /= coord[3];
                coord[1] /= coord[3];
                coord[2] /= coord[3];
            }
            sample(user, (w[0] >> 17) & 0xF, coord, r);
        } else {
            fp_vector(op, v[0], v[1], v[2], r);
        }
        if (w[0] >> 31) {
            for (i = 0; i < 4; i++) r[i] = r[i] < 0 ? 0 : r[i] > 1 ? 1 : r[i];
        }
        for (i = 0; i < 4; i++) {
            if ((w[0] >> (9 + i)) & 1) {
                t[dst][i] = r[i];
            }
        }
        if (w[0] & 1) {
            break;
        }
    }
    memcpy(out, t[0], 16);
    if (depth != NULL) {
        *depth = t[1][2];
    }
}
