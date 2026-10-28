// setjmp and longjmp with 64-bit stores, linked before newlib's, which saves the GPRs with 32-bit stores.
// They keep r1, r2, LR, CR, r13-r31 and f14-f31: 328 bytes of newlib's 512-byte jmp_buf, no vectors.

#define FUNC(name) \
    .section ".opd", "aw"; \
    .align 3; \
    .globl name; \
name: \
    .quad .name, .TOC.@tocbase, 0; \
    .previous; \
    .globl .name; \
    .type .name, @function; \
.name:

    .text

FUNC(setjmp)
    mflr 0
    std 1, 0(3)
    std 2, 8(3)
    std 0, 16(3)
    mfcr 0
    std 0, 24(3)
    std 13, 32(3)
    std 14, 40(3)
    std 15, 48(3)
    std 16, 56(3)
    std 17, 64(3)
    std 18, 72(3)
    std 19, 80(3)
    std 20, 88(3)
    std 21, 96(3)
    std 22, 104(3)
    std 23, 112(3)
    std 24, 120(3)
    std 25, 128(3)
    std 26, 136(3)
    std 27, 144(3)
    std 28, 152(3)
    std 29, 160(3)
    std 30, 168(3)
    std 31, 176(3)
    stfd 14, 184(3)
    stfd 15, 192(3)
    stfd 16, 200(3)
    stfd 17, 208(3)
    stfd 18, 216(3)
    stfd 19, 224(3)
    stfd 20, 232(3)
    stfd 21, 240(3)
    stfd 22, 248(3)
    stfd 23, 256(3)
    stfd 24, 264(3)
    stfd 25, 272(3)
    stfd 26, 280(3)
    stfd 27, 288(3)
    stfd 28, 296(3)
    stfd 29, 304(3)
    stfd 30, 312(3)
    stfd 31, 320(3)
    li 3, 0
    blr

FUNC(longjmp)
    ld 1, 0(3)
    ld 2, 8(3)
    ld 0, 16(3)
    mtlr 0
    ld 0, 24(3)
    mtcrf 0xFF, 0
    ld 13, 32(3)
    ld 14, 40(3)
    ld 15, 48(3)
    ld 16, 56(3)
    ld 17, 64(3)
    ld 18, 72(3)
    ld 19, 80(3)
    ld 20, 88(3)
    ld 21, 96(3)
    ld 22, 104(3)
    ld 23, 112(3)
    ld 24, 120(3)
    ld 25, 128(3)
    ld 26, 136(3)
    ld 27, 144(3)
    ld 28, 152(3)
    ld 29, 160(3)
    ld 30, 168(3)
    ld 31, 176(3)
    lfd 14, 184(3)
    lfd 15, 192(3)
    lfd 16, 200(3)
    lfd 17, 208(3)
    lfd 18, 216(3)
    lfd 19, 224(3)
    lfd 20, 232(3)
    lfd 21, 240(3)
    lfd 22, 248(3)
    lfd 23, 256(3)
    lfd 24, 264(3)
    lfd 25, 272(3)
    lfd 26, 280(3)
    lfd 27, 288(3)
    lfd 28, 296(3)
    lfd 29, 304(3)
    lfd 30, 312(3)
    lfd 31, 320(3)
    // longjmp(buf, 0) returns 1 from setjmp.
    mr. 3, 4
    bnelr
    li 3, 1
    blr
