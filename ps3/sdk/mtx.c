// The SDK's paired-single matrix and vector calls (PSMTX*, PSVEC*) in C, after the SDK's own C versions in
// libs/dolphin/src/dolphin/mtx, with the C-only calls of mtx.c that share a file with assembly there.
#include <math.h>
#include <string.h>

#include <dolphin/mtx.h>

#include <ps3_trig.h>

void PSMTXIdentity(Mtx m)
{
    memset(m, 0, sizeof(Mtx));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

void PSMTXCopy(Mtx src, Mtx dst)
{
    if (src != dst) {
        memcpy(dst, src, sizeof(Mtx));
    }
}

// ab may be a or b: all of a and b is read before ab is written. The fmas are the contraction GCC gave the plain
// sums, pinned for both compilers; a, read first, stays the first operand, which decides the NaN that passes.
void PSMTXConcat(Mtx a, Mtx b, Mtx ab)
{
    f32 a00 = a[0][0], a01 = a[0][1], a02 = a[0][2], a03 = a[0][3];
    f32 a10 = a[1][0], a11 = a[1][1], a12 = a[1][2], a13 = a[1][3];
    f32 a20 = a[2][0], a21 = a[2][1], a22 = a[2][2], a23 = a[2][3];
    f32 b00 = b[0][0], b01 = b[0][1], b02 = b[0][2], b03 = b[0][3];
    f32 b10 = b[1][0], b11 = b[1][1], b12 = b[1][2], b13 = b[1][3];
    f32 b20 = b[2][0], b21 = b[2][1], b22 = b[2][2], b23 = b[2][3];

#define CONCAT_ROW(i, a0, a1, a2, a3)                                                                                  \
    ab[i][0] = __builtin_fmaf(a2, b20, __builtin_fmaf(a0, b00, a1 * b10));                                            \
    ab[i][1] = __builtin_fmaf(a2, b21, __builtin_fmaf(a0, b01, a1 * b11));                                            \
    ab[i][2] = __builtin_fmaf(a2, b22, __builtin_fmaf(a0, b02, a1 * b12));                                            \
    ab[i][3] = __builtin_fmaf(a2, b23, __builtin_fmaf(a0, b03, a1 * b13)) + a3;
    CONCAT_ROW(0, a00, a01, a02, a03)
    CONCAT_ROW(1, a10, a11, a12, a13)
    CONCAT_ROW(2, a20, a21, a22, a23)
#undef CONCAT_ROW
}

void PSMTXTranspose(Mtx src, Mtx xPose)
{
    Mtx t;
    int i, j;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            t[i][j] = src[j][i];
        }
        t[i][3] = 0.0f;
    }
    memcpy(xPose, t, sizeof(Mtx));
}

u32 PSMTXInverse(Mtx src, Mtx inv)
{
    Mtx m;
    f32 det;

    det = src[2][1] * (src[0][2] * src[1][0]) + src[2][2] * (src[0][0] * src[1][1]) +
          src[2][0] * (src[0][1] * src[1][2]) - src[0][2] * (src[2][0] * src[1][1]) -
          src[2][2] * (src[1][0] * src[0][1]) - src[1][2] * (src[0][0] * src[2][1]);
    if (det == 0.0f) {
        return 0;
    }
    det = 1.0f / det;
    m[0][0] = det * (src[1][1] * src[2][2] - src[2][1] * src[1][2]);
    m[0][1] = det * -(src[0][1] * src[2][2] - src[2][1] * src[0][2]);
    m[0][2] = det * (src[0][1] * src[1][2] - src[1][1] * src[0][2]);
    m[1][0] = det * -(src[1][0] * src[2][2] - src[2][0] * src[1][2]);
    m[1][1] = det * (src[0][0] * src[2][2] - src[2][0] * src[0][2]);
    m[1][2] = det * -(src[0][0] * src[1][2] - src[1][0] * src[0][2]);
    m[2][0] = det * (src[1][0] * src[2][1] - src[2][0] * src[1][1]);
    m[2][1] = det * -(src[0][0] * src[2][1] - src[2][0] * src[0][1]);
    m[2][2] = det * (src[0][0] * src[1][1] - src[1][0] * src[0][1]);
    m[0][3] = -m[0][0] * src[0][3] - m[0][1] * src[1][3] - m[0][2] * src[2][3];
    m[1][3] = -m[1][0] * src[0][3] - m[1][1] * src[1][3] - m[1][2] * src[2][3];
    m[2][3] = -m[2][0] * src[0][3] - m[2][1] * src[1][3] - m[2][2] * src[2][3];
    memcpy(inv, m, sizeof(Mtx));
    return 1;
}

static void rot_trig(Mtx m, char axis, f32 sinA, f32 cosA)
{
    PSMTXIdentity(m);
    switch (axis) {
    case 'x':
    case 'X':
        m[1][1] = cosA;
        m[1][2] = -sinA;
        m[2][1] = sinA;
        m[2][2] = cosA;
        break;
    case 'y':
    case 'Y':
        m[0][0] = cosA;
        m[0][2] = sinA;
        m[2][0] = -sinA;
        m[2][2] = cosA;
        break;
    case 'z':
    case 'Z':
        m[0][0] = cosA;
        m[0][1] = -sinA;
        m[1][0] = sinA;
        m[1][1] = cosA;
        break;
    }
}

void MTXRotRad(Mtx m, char axis, f32 rad)
{
    rot_trig(m, axis, sinf(rad), cosf(rad));
}

void PSMTXRotAxisRad(Mtx m, Vec* axis, f32 rad)
{
    f32 s = sinf(rad);
    f32 c = cosf(rad);
    f32 t = 1.0f - c;
    Vec n;

    PSVECNormalize(axis, &n);
    m[0][0] = c + t * n.x * n.x;
    m[0][1] = n.y * (t * n.x) - s * n.z;
    m[0][2] = n.z * (t * n.x) + s * n.y;
    m[0][3] = 0.0f;
    m[1][0] = n.y * (t * n.x) + s * n.z;
    m[1][1] = c + t * n.y * n.y;
    m[1][2] = n.z * (t * n.y) - s * n.x;
    m[1][3] = 0.0f;
    m[2][0] = n.z * (t * n.x) - s * n.y;
    m[2][1] = n.z * (t * n.y) + s * n.x;
    m[2][2] = c + t * n.z * n.z;
    m[2][3] = 0.0f;
}

void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT)
{
    PSMTXIdentity(m);
    m[0][3] = xT;
    m[1][3] = yT;
    m[2][3] = zT;
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS)
{
    memset(m, 0, sizeof(Mtx));
    m[0][0] = xS;
    m[1][1] = yS;
    m[2][2] = zS;
}

void PSMTXQuat(Mtx m, Quaternion* q)
{
    f32 s = 2.0f / (q->w * q->w + q->z * q->z + q->x * q->x + q->y * q->y);
    f32 xs = q->x * s, ys = q->y * s, zs = q->z * s;
    f32 wx = q->w * xs, wy = q->w * ys, wz = q->w * zs;
    f32 xx = q->x * xs, xy = q->x * ys, xz = q->x * zs;
    f32 yy = q->y * ys, yz = q->y * zs, zz = q->z * zs;

    m[0][0] = 1.0f - (yy + zz);
    m[0][1] = xy - wz;
    m[0][2] = xz + wy;
    m[0][3] = 0.0f;
    m[1][0] = xy + wz;
    m[1][1] = 1.0f - (xx + zz);
    m[1][2] = yz - wx;
    m[1][3] = 0.0f;
    m[2][0] = xz - wy;
    m[2][1] = yz + wx;
    m[2][2] = 1.0f - (xx + yy);
    m[2][3] = 0.0f;
}

void PSMTXMultVec(Mtx44 m, Vec* src, Vec* dst)
{
    Vec v;

    v.x = m[0][0] * src->x + m[0][1] * src->y + m[0][2] * src->z + m[0][3];
    v.y = m[1][0] * src->x + m[1][1] * src->y + m[1][2] * src->z + m[1][3];
    v.z = m[2][0] * src->x + m[2][1] * src->y + m[2][2] * src->z + m[2][3];
    *dst = v;
}

void PSMTXMultVecSR(Mtx44 m, Vec* src, Vec* dst)
{
    Vec v;

    v.x = m[0][0] * src->x + m[0][1] * src->y + m[0][2] * src->z;
    v.y = m[1][0] * src->x + m[1][1] * src->y + m[1][2] * src->z;
    v.z = m[2][0] * src->x + m[2][1] * src->y + m[2][2] * src->z;
    *dst = v;
}

void C_MTXLookAt(Mtx m, Point3dPtr camPos, VecPtr camUp, Point3dPtr target)
{
    Vec look, right, up;

    look.x = camPos->x - target->x;
    look.y = camPos->y - target->y;
    look.z = camPos->z - target->z;
    PSVECNormalize(&look, &look);
    PSVECCrossProduct(camUp, &look, &right);
    PSVECNormalize(&right, &right);
    PSVECCrossProduct(&look, &right, &up);
    m[0][0] = right.x;
    m[0][1] = right.y;
    m[0][2] = right.z;
    m[0][3] = -(camPos->x * right.x + camPos->y * right.y + camPos->z * right.z);
    m[1][0] = up.x;
    m[1][1] = up.y;
    m[1][2] = up.z;
    m[1][3] = -(camPos->x * up.x + camPos->y * up.y + camPos->z * up.z);
    m[2][0] = look.x;
    m[2][1] = look.y;
    m[2][2] = look.z;
    m[2][3] = -(camPos->x * look.x + camPos->y * look.y + camPos->z * look.z);
}

void MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 scaleS, f32 scaleT, f32 transS,
                     f32 transT)
{
    f32 tmp = 1.0f / (r - l);

    memset(m, 0, sizeof(Mtx));
    m[0][0] = scaleS * (2.0f * n * tmp);
    m[0][2] = scaleS * (tmp * (r + l)) - transS;
    tmp = 1.0f / (t - b);
    m[1][1] = scaleT * (2.0f * n * tmp);
    m[1][2] = scaleT * (tmp * (t + b)) - transT;
    m[2][2] = -1.0f;
}

void MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT)
{
    f32 cot = 1.0f / tanf(0.5f * fovY * 0.017453293f);

    memset(m, 0, sizeof(Mtx));
    m[0][0] = scaleS * (cot / aspect);
    m[0][2] = -transS;
    m[1][1] = cot * scaleT;
    m[1][2] = -transT;
    m[2][2] = -1.0f;
}

void MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT, f32 transS, f32 transT)
{
    f32 tmp = 1.0f / (r - l);

    memset(m, 0, sizeof(Mtx));
    m[0][0] = 2.0f * tmp * scaleS;
    m[0][3] = transS + scaleS * (tmp * -(r + l));
    tmp = 1.0f / (t - b);
    m[1][1] = 2.0f * tmp * scaleT;
    m[1][3] = transT + scaleT * (tmp * -(t + b));
    m[2][3] = 1.0f;
}

void PSVECAdd(Vec* a, Vec* b, Vec* c)
{
    c->x = a->x + b->x;
    c->y = a->y + b->y;
    c->z = a->z + b->z;
}

void PSVECSubtract(Vec* a, Vec* b, Vec* c)
{
    c->x = a->x - b->x;
    c->y = a->y - b->y;
    c->z = a->z - b->z;
}

void PSVECScale(Vec* src, Vec* dst, f32 scale)
{
    dst->x = src->x * scale;
    dst->y = src->y * scale;
    dst->z = src->z * scale;
}

void PSVECNormalize(Vec* src, Vec* unit)
{
    f32 mag = 1.0f / sqrtf(src->x * src->x + src->y * src->y + src->z * src->z);

    unit->x = src->x * mag;
    unit->y = src->y * mag;
    unit->z = src->z * mag;
}

f32 PSVECMag(Vec* v)
{
    return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
}

f32 PSVECDotProduct(Vec* a, Vec* b)
{
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

// axb may be a or b.
void PSVECCrossProduct(Vec* a, Vec* b, Vec* axb)
{
    Vec v;

    v.x = a->y * b->z - a->z * b->y;
    v.y = a->z * b->x - a->x * b->z;
    v.z = a->x * b->y - a->y * b->x;
    *axb = v;
}
