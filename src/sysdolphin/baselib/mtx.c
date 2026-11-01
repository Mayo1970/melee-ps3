#include "mtx.h"

#include <math.h>

#include "debug.h"

#define EPSILON 0.0000000001f
#define FLOAT_MIN 1.1754943E-38f

HSD_ObjAllocData HSD_Mtx_804C2310;
HSD_ObjAllocData HSD_Mtx_804C233C;

/// Calculates the determinant of the top 3x3 section of a 3x4 matrix
static inline f32 HSD_CalcDeterminantMatrix3x4(Mtx m)
{
    return m[0][0] * m[1][1] * m[2][2] + m[0][1] * m[1][2] * m[2][0] +
           m[0][2] * m[1][0] * m[2][1] - m[2][0] * m[1][1] * m[0][2] -
           m[1][0] * m[0][1] * m[2][2] - m[0][0] * m[2][1] * m[1][2];
}

void HSD_MtxInverse(Mtx src, Mtx dest)
{
    Mtx tempMatrix;
    Mtx* m;
    f32 det = HSD_CalcDeterminantMatrix3x4(src);

    if (fabsf_bitwise(det) < EPSILON) {
        MTXIdentity(dest);
        return;
    }

    if (src == dest) {
        MTXCopy(src, tempMatrix);
        m = &tempMatrix;
    } else {
        m = (Mtx*) src;
    }

    det = 1.0f / det;

    dest[0][0] = ((*m)[1][1] * (*m)[2][2] - (*m)[2][1] * (*m)[1][2]) * det;
    dest[0][1] = -((*m)[0][1] * (*m)[2][2] - (*m)[2][1] * (*m)[0][2]) * det;
    dest[0][2] = ((*m)[0][1] * (*m)[1][2] - (*m)[1][1] * (*m)[0][2]) * det;
    dest[1][0] = -((*m)[1][0] * (*m)[2][2] - (*m)[2][0] * (*m)[1][2]) * det;
    dest[1][1] = ((*m)[0][0] * (*m)[2][2] - (*m)[2][0] * (*m)[0][2]) * det;
    dest[1][2] = -((*m)[0][0] * (*m)[1][2] - (*m)[1][0] * (*m)[0][2]) * det;
    dest[2][0] = ((*m)[1][0] * (*m)[2][1] - (*m)[2][0] * (*m)[1][1]) * det;
    dest[2][1] = -((*m)[0][0] * (*m)[2][1] - (*m)[2][0] * (*m)[0][1]) * det;
    dest[2][2] = ((*m)[0][0] * (*m)[1][1] - (*m)[1][0] * (*m)[0][1]) * det;

    dest[0][3] = -(dest[0][2] * src[2][3] -
                   (-dest[0][0] * src[0][3] - dest[0][1] * src[1][3]));
    dest[1][3] = -(dest[1][2] * src[2][3] -
                   (-dest[1][0] * src[0][3] - dest[1][1] * src[1][3]));
    dest[2][3] = -(dest[2][2] * src[2][3] -
                   (-dest[2][0] * src[0][3] - dest[2][1] * src[1][3]));
}

void HSD_MtxInverseConcat(Mtx inv, Mtx src, Mtx dest)
{
    Mtx m;
    f32 det;
    f32 temp1;
    f32 temp2;
    f32 temp3;
    f32 temp4;
    f32 temp5;
    f32 temp6;
    f32 temp7;
    f32 temp8;
    f32 temp9;
    f32 temp10;
    f32 temp11;
    f32 temp12;
    f32 new_var; ///< @todo try to get rid of this

    det = HSD_CalcDeterminantMatrix3x4(inv);

    if (fabsf_bitwise(det) < EPSILON) {
        if (src != dest) {
            MTXCopy(src, dest);
        }
    } else {
        det = 1.0f / det;
        temp1 = ((inv[1][1] * inv[2][2]) - (inv[2][1] * inv[1][2])) * det;
        temp2 = (-((inv[0][1] * inv[2][2]) - (inv[2][1] * inv[0][2]))) * det;
        new_var = inv[1][1];
        temp3 = (-((inv[1][0] * inv[2][2]) - (inv[2][0] * inv[1][2]))) * det;
        temp7 = ((inv[0][1] * inv[1][2]) - (new_var * inv[0][2])) * det;
        temp4 = ((inv[0][0] * inv[2][2]) - (inv[2][0] * inv[0][2])) * det;
        temp8 = (-((inv[0][0] * inv[1][2]) - (inv[1][0] * inv[0][2]))) * det;
        temp5 = ((inv[1][0] * inv[2][1]) - (inv[2][0] * new_var)) * det;
        temp6 = (-((inv[0][0] * inv[2][1]) - (inv[2][0] * inv[0][1]))) * det;
        temp9 = ((inv[0][0] * inv[1][1]) - (inv[1][0] * inv[0][1])) * det;
        temp10 = -((temp7 * inv[2][3]) -
                   (((-temp1) * inv[0][3]) - (temp2 * inv[1][3])));
        temp11 = -((temp8 * inv[2][3]) -
                   (((-temp3) * inv[0][3]) - (temp4 * inv[1][3])));
        temp12 = -((temp9 * inv[2][3]) -
                   (((-temp5) * (new_var = inv[0][3])) - (temp6 * inv[1][3])));

        if (inv == dest || src == dest) {
            m[0][0] =
                temp7 * src[2][0] + (temp1 * src[0][0] + temp2 * src[1][0]);
            m[0][1] =
                temp7 * src[2][1] + (temp1 * src[0][1] + temp2 * src[1][1]);
            m[0][2] =
                temp7 * src[2][2] + (temp1 * src[0][2] + temp2 * src[1][2]);
            m[0][3] = temp7 * src[2][3] +
                      (temp1 * src[0][3] + temp2 * src[1][3]) + temp10;
            m[1][0] =
                temp8 * src[2][0] + (temp3 * src[0][0] + temp4 * src[1][0]);
            m[1][1] =
                temp8 * src[2][1] + (temp3 * src[0][1] + temp4 * src[1][1]);
            m[1][2] =
                temp8 * src[2][2] + (temp3 * src[0][2] + temp4 * src[1][2]);
            m[1][3] = temp8 * src[2][3] +
                      (temp3 * src[0][3] + temp4 * src[1][3]) + temp11;
            m[2][0] =
                temp9 * src[2][0] + (temp5 * src[0][0] + temp6 * src[1][0]);
            m[2][1] =
                temp9 * src[2][1] + (temp5 * src[0][1] + temp6 * src[1][1]);
            m[2][2] =
                temp9 * src[2][2] + (temp5 * src[0][2] + temp6 * src[1][2]);
            m[2][3] = temp9 * src[2][3] +
                      (temp5 * src[0][3] + temp6 * src[1][3]) + temp12;

            MTXCopy(m, dest);
        } else {
            dest[0][0] =
                temp7 * src[2][0] + (temp1 * src[0][0] + temp2 * src[1][0]);
            dest[0][1] =
                temp7 * src[2][1] + (temp1 * src[0][1] + temp2 * src[1][1]);
            dest[0][2] =
                temp7 * src[2][2] + (temp1 * src[0][2] + temp2 * src[1][2]);
            dest[0][3] = temp7 * src[2][3] +
                         (temp1 * src[0][3] + temp2 * src[1][3]) + temp10;
            dest[1][0] =
                temp8 * src[2][0] + (temp3 * src[0][0] + temp4 * src[1][0]);
            dest[1][1] =
                temp8 * src[2][1] + (temp3 * src[0][1] + temp4 * src[1][1]);
            dest[1][2] =
                temp8 * src[2][2] + (temp3 * src[0][2] + temp4 * src[1][2]);
            dest[1][3] = temp8 * src[2][3] +
                         (temp3 * src[0][3] + temp4 * src[1][3]) + temp11;
            dest[2][0] =
                temp9 * src[2][0] + (temp5 * src[0][0] + temp6 * src[1][0]);
            dest[2][1] =
                temp9 * src[2][1] + (temp5 * src[0][1] + temp6 * src[1][1]);
            dest[2][2] =
                temp9 * src[2][2] + (temp5 * src[0][2] + temp6 * src[1][2]);
            dest[2][3] = temp9 * src[2][3] +
                         (temp5 * src[0][3] + temp6 * src[1][3]) + temp12;
        }
    }
}

void HSD_MtxInverseTranspose(Mtx src, Mtx dest)
{
#ifdef TARGET_PS3
    // src is read once, before dest is written, so dest may be src without a copy. The fmas are the ones GCC made
    // of the #else expressions, pinned.
    f32 m00 = src[0][0], m01 = src[0][1], m02 = src[0][2];
    f32 m10 = src[1][0], m11 = src[1][1], m12 = src[1][2];
    f32 m20 = src[2][0], m21 = src[2][1], m22 = src[2][2];
    f32 det = __builtin_fmaf(
        -m12, m00 * m21,
        __builtin_fmaf(-m22, m01 * m10,
                       __builtin_fmaf(-m02, m11 * m20,
                                      __builtin_fmaf(m02 * m10, m21,
                                                     __builtin_fmaf(m00 * m11, m22, (m01 * m12) * m20)))));

    if (fabsf_bitwise(det) < EPSILON) {
        if (src != dest) {
            MTXCopy(src, dest);
        }
        return;
    }
    det = 1.0f / det;
    // GCC orders a product's operands by definition; copies defined in this order give the #else code's operand
    // orders, which decide the NaN that passes.
    __asm__("" : "+f"(m20), "+f"(m10), "+f"(m00), "+f"(m21), "+f"(m11), "+f"(m01), "+f"(m12), "+f"(m02), "+f"(m22));
    dest[0][0] = __builtin_fmaf(m11, m22, -(m21 * m12)) * det;
    dest[1][0] = -__builtin_fmaf(m01, m22, -(m21 * m02)) * det;
    dest[2][0] = __builtin_fmaf(m01, m12, -(m11 * m02)) * det;
    dest[0][1] = -__builtin_fmaf(m10, m22, -(m20 * m12)) * det;
    dest[1][1] = __builtin_fmaf(m00, m22, -(m20 * m02)) * det;
    dest[2][1] = -__builtin_fmaf(m00, m12, -(m10 * m02)) * det;
    dest[0][2] = __builtin_fmaf(m10, m21, -(m20 * m11)) * det;
    dest[1][2] = -__builtin_fmaf(m00, m21, -(m20 * m01)) * det;
    dest[2][2] = __builtin_fmaf(m00, m11, -(m10 * m01)) * det;
    dest[0][3] = 0;
    dest[1][3] = 0;
    dest[2][3] = 0;
#else
    Mtx* m;
    Mtx tempMatrix;
    f32 det = HSD_CalcDeterminantMatrix3x4(src);

    m = (Mtx*) src;

    if (fabsf_bitwise(det) < EPSILON) {
        if (*m != dest) {
            MTXCopy(*m, dest);
        }
    } else {
        if (*m == dest) {
            MTXCopy(*m, tempMatrix);
            m = &tempMatrix;
        }

        det = 1.0f / det;

        // This needs to be in a different order than in HSD_MtxInverse for
        // some reason
        dest[0][0] =
            (((*m)[1][1] * (*m)[2][2]) - ((*m)[2][1] * (*m)[1][2])) * det;
        dest[1][0] =
            -(((*m)[0][1] * (*m)[2][2]) - ((*m)[2][1] * (*m)[0][2])) * det;
        dest[2][0] =
            (((*m)[0][1] * (*m)[1][2]) - ((*m)[1][1] * (*m)[0][2])) * det;
        dest[0][1] =
            -(((*m)[1][0] * (*m)[2][2]) - ((*m)[2][0] * (*m)[1][2])) * det;
        dest[1][1] =
            (((*m)[0][0] * (*m)[2][2]) - ((*m)[2][0] * (*m)[0][2])) * det;
        dest[2][1] =
            -(((*m)[0][0] * (*m)[1][2]) - ((*m)[1][0] * (*m)[0][2])) * det;
        dest[0][2] =
            (((*m)[1][0] * (*m)[2][1]) - ((*m)[2][0] * (*m)[1][1])) * det;
        dest[1][2] =
            -(((*m)[0][0] * (*m)[2][1]) - ((*m)[2][0] * (*m)[0][1])) * det;
        dest[2][2] =
            (((*m)[0][0] * (*m)[1][1]) - ((*m)[1][0] * (*m)[0][1])) * det;
        dest[0][3] = 0;
        dest[1][3] = 0;
        dest[2][3] = 0;
    }
#endif
}

static inline f32 calcVal(f32 x, f32 y)
{
    if (fabsf_bitwise(x) <= FLOAT_MIN) {
        if (y >= 0) {
            return M_PI / 2;
        } else {
            return -M_PI / 2;
        }
    } else {
        return atan2f(y, x);
    }
}

void HSD_MtxGetRotation(Mtx m, Vec3* vec)
{
    f32 length0;
    f32 length1;
    f32 length2;
    f32 testVal_1;
    f32 val_01;

    length0 = sqrtf(m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0]);
    if (!(length0 < FLOAT_MIN)) {
        length1 =
            sqrtf(m[0][1] * m[0][1] + m[1][1] * m[1][1] + m[2][1] * m[2][1]);
        if (!(length1 < FLOAT_MIN)) {
            length2 = sqrtf(m[0][2] * m[0][2] + m[1][2] * m[1][2] +
                            m[2][2] * m[2][2]);
            if (!(length2 < FLOAT_MIN)) {
                testVal_1 = -m[2][0];
                testVal_1 /= length0;

                if (testVal_1 >= 1.0f) {
                    val_01 = M_PI / 2;
                } else if (testVal_1 <= -1) {
                    val_01 = -M_PI / 2;
                } else {
                    val_01 = asinf(testVal_1);
                }

                vec->y = val_01;

                if (cosf(vec->y) >= FLOAT_MIN) {
                    f32 testVal_2_pre = m[2][2] / length2;
                    f32 testVal_3_pre = m[2][1] / length1;

                    vec->x = calcVal(testVal_2_pre, testVal_3_pre);
                    vec->z = calcVal(m[0][0], m[1][0]);
                    return;
                }

                vec->x = calcVal(m[1][1], m[0][1]);
                vec->z = 0;
                return;
            }
        }
    }

    vec->x = 0;
    vec->y = 0;
    vec->z = 0;
}

/// These parameters may not be right
void HSD_MtxGetTranslate(Mtx mat, Vec3* vec)
{
    vec->x = mat[0][3];
    vec->y = mat[1][3];
    vec->z = mat[2][3];
}

void HSD_MtxGetScale(Mtx arg0, Vec3* arg1)
{
    f64 scale;

    u8 _[8];

    Vec3 vec1;
    Vec3 vec2;
    Vec3 vec3;
    Vec3 vec4;

    vec1.x = arg0[0][0];
    vec1.y = arg0[1][0];
    vec1.z = arg0[2][0];

    arg1->x = VECMag(&vec1);
    VECNormalize(&vec1, &vec1);

    vec2.x = arg0[0][1];
    vec2.y = arg0[1][1];
    vec2.z = arg0[2][1];

    VECScale(&vec1, &vec4, VECDotProduct(&vec1, &vec2));
    VECSubtract(&vec2, &vec4, &vec2);
    arg1->y = VECMag(&vec2);
    VECNormalize(&vec2, &vec2);

    vec3.x = arg0[0][2];
    vec3.y = arg0[1][2];
    vec3.z = arg0[2][2];

    VECScale(&vec2, &vec4, VECDotProduct(&vec2, &vec3));
    VECSubtract(&vec3, &vec4, &vec3);
    VECScale(&vec1, &vec4, VECDotProduct(&vec1, &vec3));
    VECSubtract(&vec3, &vec4, &vec3);
    arg1->z = VECMag(&vec3);
    VECNormalize(&vec3, &vec3);
    VECCrossProduct(&vec2, &vec3, &vec4);

    if (VECDotProduct(&vec1, &vec4) < 0.0) {
        scale = -1.0;
        arg1->x *= scale;
        arg1->y *= scale;
        arg1->z *= scale;
    }
}

void HSD_MkRotationMtx(Mtx arg0, Vec3* arg1)
{
    f32 sinX;
    f32 cosX;
    f32 sinY;
    f32 cosY;
    f32 sinZ;
    f32 cosZ;
    f32 temp1;
    f32 temp2;

    sinX = sinf(arg1->x);
    cosX = cosf(arg1->x);
    sinY = sinf(arg1->y);
    cosY = cosf(arg1->y);
    sinZ = sinf(arg1->z);
    cosZ = cosf(arg1->z);

    temp1 = sinX * sinY;
    arg0[0][0] = cosY * cosZ;
    arg0[1][0] = cosY * sinZ;
    arg0[2][0] = -sinY;
    temp2 = cosX * sinY;
    arg0[0][1] = (cosZ * temp1) - (cosX * sinZ);
    arg0[1][1] = (sinZ * temp1) + (cosX * cosZ);
    arg0[2][1] = sinX * cosY;
    arg0[0][2] = (cosZ * temp2) + (sinX * sinZ);
    arg0[1][2] = (sinZ * temp2) - (sinX * cosZ);
    arg0[2][2] = cosX * cosY;
    arg0[0][3] = 0;
    arg0[1][3] = 0;
    arg0[2][3] = 0;
}

void HSD_MtxQuat(Mtx arg0, Quaternion* arg1)
{
    MTXQuat(arg0, arg1);
}

void HSD_MtxSRT(Mtx m, Vec3* vec1, Vec3* vec2, Vec3* vec3, Vec3* vec4)
{
    f32 vec1x_2;
    f32 vec1y_2;
    f32 vec1z_2;
    f32 vec1x_1;
    f32 vec1y_1;
    f32 vec1z_1;
    f32 vec1x;
    f32 vec1y;
    f32 vec1z;

    f32 sinX = sinf(vec2->x);
    f32 cosX = cosf(vec2->x);
    f32 sinY = sinf(vec2->y);
    f32 cosY = cosf(vec2->y);
    f32 sinZ = sinf(vec2->z);
    f32 cosZ = cosf(vec2->z);

    vec1x_2 = vec1x_1 = vec1x = vec1->x;
    vec1y_2 = vec1y_1 = vec1y = vec1->y;
    vec1z_2 = vec1z_1 = vec1z = vec1->z;

    if (vec4 != NULL) {
        f32 temp1 = 1.0 / vec4->x;
        f32 temp2 = 1.0 / vec4->y;
        f32 temp3 = 1.0 / vec4->z;

        vec1y_2 *= vec4->y * temp1;
        vec1z_2 *= vec4->z * temp1;
        vec1x_1 *= vec4->x * temp2;
        vec1z_1 *= vec4->z * temp2;
        vec1x *= vec4->x * temp3;
        vec1y *= vec4->y * temp3;
    }

    m[0][0] = cosZ * (vec1x_2 * cosY);
    m[1][0] = sinZ * (vec1x_1 * cosY);
    m[2][0] = -vec1x * sinY;
    m[0][1] = vec1y_2 * ((cosZ * (sinX * sinY)) - (cosX * sinZ));
    m[1][1] = vec1y_1 * ((sinZ * (sinX * sinY)) + (cosX * cosZ));
    m[2][1] = cosY * (vec1y * sinX);
    m[0][2] = vec1z_2 * ((cosZ * (cosX * sinY)) + (sinX * sinZ));
    m[1][2] = vec1z_1 * ((sinZ * (cosX * sinY)) - (sinX * cosZ));
    m[2][2] = cosY * (vec1z * cosX);
    m[0][3] = vec3->x;
    m[1][3] = vec3->y;
    m[2][3] = vec3->z;
}

void HSD_MtxSRTQuat(Mtx arg0, Vec3* arg1, Quaternion* arg2, Vec3* arg3,
                    Vec3* arg4)
{
    Mtx temp;

    MTXScale(arg0, arg1->x, arg1->y, arg1->z);

    if (arg4 != NULL) {
        MTXScale(temp, arg4->x, arg4->y, arg4->z);
        MTXConcat(temp, arg0, arg0);
    }

    MTXQuat(temp, arg2);
    MTXConcat(temp, arg0, arg0);

    if (arg4 != NULL) {
        MTXScale(temp, 1.0 / arg4->x, 1.0 / arg4->y, 1.0 / arg4->z);
        MTXConcat(temp, arg0, arg0);
    }

    PSMTXTrans(temp, arg3->x, arg3->y, arg3->z);
    MTXConcat(temp, arg0, arg0);
}

/// might be a fakematch?
void HSD_MtxScaledAdd(Mtx arg0, Mtx arg1, Mtx arg2, f32 arg3)
{
#ifdef TARGET_PS3
    // All loads before the first store, so the twelve fmas overlap; arg2 is arg1 at the callers.
    f32* arr0 = &arg0[0][0];
    f32* arr1 = &arg1[0][0];
    f32* arr2 = &arg2[0][0];
    f32 r0 = __builtin_fmaf(arr0[0], arg3, arr1[0]), r1 = __builtin_fmaf(arr0[1], arg3, arr1[1]);
    f32 r2 = __builtin_fmaf(arr0[2], arg3, arr1[2]), r3 = __builtin_fmaf(arr0[3], arg3, arr1[3]);
    f32 r4 = __builtin_fmaf(arr0[4], arg3, arr1[4]), r5 = __builtin_fmaf(arr0[5], arg3, arr1[5]);
    f32 r6 = __builtin_fmaf(arr0[6], arg3, arr1[6]), r7 = __builtin_fmaf(arr0[7], arg3, arr1[7]);
    f32 r8 = __builtin_fmaf(arr0[8], arg3, arr1[8]), r9 = __builtin_fmaf(arr0[9], arg3, arr1[9]);
    f32 r10 = __builtin_fmaf(arr0[10], arg3, arr1[10]), r11 = __builtin_fmaf(arr0[11], arg3, arr1[11]);

    arr2[0] = r0; arr2[1] = r1; arr2[2] = r2; arr2[3] = r3; arr2[4] = r4; arr2[5] = r5;
    arr2[6] = r6; arr2[7] = r7; arr2[8] = r8; arr2[9] = r9; arr2[10] = r10; arr2[11] = r11;
#else
    f32* arr0 = (&arg0[0][0]);
    f32* arr1 = (&arg1[0][0]);
    f32* arr2 = (&arg2[0][0]);

    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);

    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);

    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
#endif
}

void* HSD_VecAlloc(void)
{
    void* vec = HSD_ObjAlloc(&HSD_Mtx_804C2310);

    HSD_ASSERT(0x335, vec);

    return vec;
}

void HSD_VecFree(void* arg0)
{
    if (arg0 != NULL) {
        HSD_ObjFree(&HSD_Mtx_804C2310, arg0);
    }
}

void* HSD_MtxAlloc(void)
{
    void* mtx;

    mtx = HSD_ObjAlloc(&HSD_Mtx_804C233C);
    HSD_ASSERT(0x354, mtx);
    return mtx;
}

void HSD_MtxFree(void* arg0)
{
    if (arg0 != NULL) {
        HSD_ObjFree(&HSD_Mtx_804C233C, arg0);
    }
}

HSD_ObjAllocData* HSD_VecGetAllocData(void)
{
    return &HSD_Mtx_804C2310;
}

void HSD_VecInitAllocData(void)
{
    HSD_ObjAllocInit(HSD_VecGetAllocData(), sizeof(Vec), 4);
}

HSD_ObjAllocData* HSD_MtxGetAllocData(void)
{
    return &HSD_Mtx_804C233C;
}

void HSD_MtxInitAllocData(void)
{
    HSD_ObjAllocInit(HSD_MtxGetAllocData(), sizeof(Mtx), 4);
}
