// SPDX-License-Identifier: AGPL-3.0-only
// CTIDH-1024's field for the ESP32-S3 (Xtensa LX7), in place of fp1024_mont32.c and with its
// contract: fiat_p1024_mul, _square, _add and _sub on elements in the Montgomery domain with
// R = 2^1024, inputs and outputs fully reduced, so every result is the same bytes fiat's is.
// Windowed ABI; the output may alias an input.
//
// Constant time: fixed trip counts, fixed addresses, and no branch on data. Carries and
// borrows are computed with logic and MINU (this core has no SALTU).
//
// Multiply (prototype 1): product scanning over 32-bit words with MULL/MULUH. A column's low
// and high product words are summed mod 2^32 together with their top 16 bits, which cannot
// overflow, and the column's carries are recovered from those sums once per column.

#define N 32

    .section .rodata.fp1024_xtensa, "a"
    .align  4
.Lp:    // p, 32 words, least significant first
    .word   0x60e36453, 0xdbe34c54, 0xbc3d344d, 0xa1d81eeb, 0xb8d89fd3, 0x514ba72c, 0xe287f1bd, 0xc2cab6a0
    .word   0x5a313709, 0x642aca4d, 0x31541f40, 0x6b317c54, 0xde81ede5, 0xb97c56d1, 0xd90a2b58, 0x0978dbee
    .word   0x90441c80, 0x7611ad4f, 0x19ec8329, 0xf811d9c4, 0x8ad82d2d, 0x4d6c594a, 0x1cf9386e, 0xf06de247
    .word   0xdb31ad5b, 0x0683cf25, 0x86f21a08, 0x216c22bc, 0x9007ebd7, 0xd89dec87, 0x427012a9, 0x0ece55ed

    .section .text.fp1024_xtensa, "ax"

// out = a * b / R mod p.  a2 = out, a3 = a, a4 = b.
// Frame: X[64] = (a_i, q_i) pairs at sp, Y[64] = (p_j, b_j) pairs at sp + 256, a0 at sp + 512.
    .align  4
    .global fiat_p1024_mul
    .type   fiat_p1024_mul, @function
fiat_p1024_mul:
    entry   a1, 544
.Lmul:
    s32i    a0, a1, 512
    movi    a5, .Lp
    mov     a6, a1
    movi    a7, 0
    movi    a8, N / 4
    loop    a8, 1f
    l32i    a9, a3, 0
    l32i    a10, a4, 0
    l32i    a11, a5, 0
    s32i    a9, a6, 0
    s32i    a7, a6, 4
    s32i    a11, a6, 256
    s32i    a10, a6, 260
    l32i    a9, a3, 4
    l32i    a10, a4, 4
    l32i    a11, a5, 4
    s32i    a9, a6, 8
    s32i    a7, a6, 12
    s32i    a11, a6, 264
    s32i    a10, a6, 268
    l32i    a9, a3, 8
    l32i    a10, a4, 8
    l32i    a11, a5, 8
    s32i    a9, a6, 16
    s32i    a7, a6, 20
    s32i    a11, a6, 272
    s32i    a10, a6, 276
    l32i    a9, a3, 12
    l32i    a10, a4, 12
    l32i    a11, a5, 12
    s32i    a9, a6, 24
    s32i    a7, a6, 28
    s32i    a11, a6, 280
    s32i    a10, a6, 284
    addi    a3, a3, 16
    addi    a4, a4, 16
    addi    a5, a5, 16
    addi    a6, a6, 32
1:
    // S0, U0: the column's low words mod 2^32 and the sum of their top halves;
    // S1, U1: the same for the high words. a5 = S0, a6 = U0, a7 = S1, a8 = U1.
#define COLUMN_STEP \
    l32i    a9, a3, 0;  /* a_i */ \
    l32i    a10, a4, 4; /* b_j */ \
    l32i    a11, a3, 4; /* q_i */ \
    l32i    a12, a4, 0; /* p_j */ \
    mull    a0, a9, a10; \
    muluh   a9, a9, a10; \
    add     a5, a5, a0; \
    srli    a0, a0, 16; \
    add     a6, a6, a0; \
    add     a7, a7, a9; \
    srli    a9, a9, 16; \
    add     a8, a8, a9; \
    mull    a0, a11, a12; \
    muluh   a11, a11, a12; \
    add     a5, a5, a0; \
    srli    a0, a0, 16; \
    add     a6, a6, a0; \
    add     a7, a7, a11; \
    srli    a11, a11, 16; \
    add     a8, a8, a11; \
    addi    a3, a3, 8; \
    addi    a4, a4, -8
    // The column total is S0 + 2^32 (carry0 + S1 + 2^32 Chi); the next column starts from
    // its value shifted down 32 bits.
#define COLUMN_CARRY \
    slli    a9, a6, 16; \
    sub     a9, a5, a9;   /* the low halves' sum */ \
    srli    a9, a9, 16; \
    add     a9, a6, a9; \
    srli    a9, a9, 16;   /* carry0 */ \
    slli    a10, a8, 16; \
    sub     a10, a7, a10; \
    add     a10, a10, a9; \
    srli    a10, a10, 16; \
    add     a10, a8, a10; \
    srli    a10, a10, 16; /* Chi */ \
    add     a5, a7, a9; \
    srli    a6, a5, 16; \
    mov     a7, a10; \
    movi    a8, 0
    movi    a5, 0
    movi    a6, 0
    movi    a7, 0
    movi    a8, 0
    // Columns 0..31: sum a_i b_(k-i) and q_i p_(k-i) for i = 0..k (q_k is still 0), then pick
    // q_k so that the column is 0 mod 2^32.
    addmi   a14, a1, 256        // &Y[2k]
    addmi   a15, a1, 512
    movi    a13, 1
.Llow:
    mov     a3, a1
    mov     a4, a14
    loop    a13, 1f
    COLUMN_STEP
1:
    movi    a9, 0x60038025      // -p^-1 mod 2^32
    mull    a9, a5, a9
    addmi   a10, a14, -256
    s32i    a9, a10, 4
    movi    a10, 0x60e36453
    mull    a11, a9, a10
    muluh   a9, a9, a10
    add     a5, a5, a11
    srli    a11, a11, 16
    add     a6, a6, a11
    add     a7, a7, a9
    srli    a9, a9, 16
    add     a8, a8, a9
    COLUMN_CARRY
    addi    a14, a14, 8
    addi    a13, a13, 1
    bne     a14, a15, .Llow
    // Columns 32..63: i = k-31..31; the column's low word is the output word k-32.
    addi    a14, a1, 8          // &X[2(k-31)]
    addi    a15, a15, -8        // &Y[62]
    movi    a13, 31             // 63 - k products
.Lhigh:
    mov     a3, a14
    mov     a4, a15
    loopgtz a13, 1f
    COLUMN_STEP
1:
    s32i    a5, a2, 0
    COLUMN_CARRY
    addi    a14, a14, 8
    addi    a2, a2, 4
    addi    a13, a13, -1
    bgez    a13, .Lhigh
    // t < 2p: out = t - p unless that borrows. d goes to X, then a masked select.
    addi    a2, a2, -128
    movi    a3, .Lp
    mov     a4, a2
    mov     a5, a1
    movi    a6, 0               // borrow
    movi    a7, 1
    movi    a8, N
    loop    a8, 1f
    l32i    a9, a4, 0
    l32i    a10, a3, 0
    add     a10, a10, a6        // p_j + borrow: p has no all-ones word, so this cannot wrap
    sub     a11, a9, a10
    minu    a12, a9, a10
    sub     a12, a10, a12
    minu    a6, a12, a7         // t_j < p_j + borrow
    s32i    a11, a5, 0
    addi    a3, a3, 4
    addi    a4, a4, 4
    addi    a5, a5, 4
1:
    neg     a6, a6              // all ones: keep t
    mov     a5, a1
    movi    a8, N
    loop    a8, 1f
    l32i    a9, a2, 0
    l32i    a10, a5, 0
    xor     a9, a9, a10
    and     a9, a9, a6
    xor     a9, a9, a10
    s32i    a9, a2, 0
    addi    a2, a2, 4
    addi    a5, a5, 4
1:
    l32i    a0, a1, 512
    retw
    .size   fiat_p1024_mul, . - fiat_p1024_mul

// out = a^2 / R mod p: the multiply with b = a.
    .align  4
    .global fiat_p1024_square
    .type   fiat_p1024_square, @function
fiat_p1024_square:
    entry   a1, 544
    mov     a4, a3
    j       .Lmul
    .size   fiat_p1024_square, . - fiat_p1024_square

// out = a + b mod p. s = a + b goes to out and d = s - p to the frame; then out = d unless
// d borrowed.
    .align  4
    .global fiat_p1024_add
    .type   fiat_p1024_add, @function
fiat_p1024_add:
    entry   a1, 144
    movi    a5, .Lp
    mov     a6, a1
    movi    a7, 0               // carry
    movi    a8, 0               // borrow
    movi    a9, -1
    movi    a10, 1
    movi    a11, N
    loop    a11, 1f
    l32i    a12, a3, 0
    l32i    a13, a4, 0
    add     a14, a12, a13
    add     a14, a14, a7        // s
    or      a11, a12, a13
    and     a12, a12, a13
    xor     a15, a14, a9
    and     a11, a11, a15
    or      a11, a11, a12
    srli    a7, a11, 31         // carry out of s (Hacker's Delight 2-13)
    l32i    a15, a5, 0
    add     a15, a15, a8        // p_j + borrow cannot wrap
    sub     a11, a14, a15
    s32i    a14, a2, 0
    s32i    a11, a6, 0
    minu    a11, a14, a15
    sub     a11, a15, a11
    minu    a8, a11, a10        // s_j < p_j + borrow
    addi    a3, a3, 4
    addi    a4, a4, 4
    addi    a5, a5, 4
    addi    a6, a6, 4
    addi    a2, a2, 4
1:
    addi    a2, a2, -128
    neg     a8, a8              // all ones: s < p, keep s
    mov     a6, a1
    movi    a11, N
    loop    a11, 1f
    l32i    a12, a2, 0
    l32i    a13, a6, 0
    xor     a12, a12, a13
    and     a12, a12, a8
    xor     a12, a12, a13
    s32i    a12, a2, 0
    addi    a2, a2, 4
    addi    a6, a6, 4
1:
    retw
    .size   fiat_p1024_add, . - fiat_p1024_add

// out = a - b mod p: d = a - b, then p added back under the borrow's mask.
    .align  4
    .global fiat_p1024_sub
    .type   fiat_p1024_sub, @function
fiat_p1024_sub:
    entry   a1, 32
    mov     a5, a2
    movi    a8, 0               // borrow
    movi    a11, N
    loop    a11, 1f
    l32i    a12, a3, 0
    l32i    a13, a4, 0
    sub     a14, a12, a13
    sub     a14, a14, a8        // d
    xor     a15, a12, a13
    xor     a9, a14, a13
    and     a9, a9, a15
    xor     a9, a9, a14
    srli    a8, a9, 31          // borrow: d's top bit where a and b agree there, else b's
    s32i    a14, a5, 0
    addi    a3, a3, 4
    addi    a4, a4, 4
    addi    a5, a5, 4
1:
    neg     a8, a8
    movi    a3, .Lp
    movi    a7, 0               // carry
    movi    a10, 1
    movi    a11, N
    loop    a11, 1f
    l32i    a12, a2, 0
    l32i    a13, a3, 0
    and     a13, a13, a8
    add     a13, a13, a7        // (p_j & mask) + carry cannot wrap
    add     a14, a12, a13
    minu    a15, a14, a12
    sub     a15, a12, a15
    minu    a7, a15, a10        // the sum wrapped
    s32i    a14, a2, 0
    addi    a2, a2, 4
    addi    a3, a3, 4
1:
    retw
    .size   fiat_p1024_sub, . - fiat_p1024_sub
