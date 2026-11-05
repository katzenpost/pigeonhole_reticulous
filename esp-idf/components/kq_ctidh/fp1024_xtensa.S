// SPDX-License-Identifier: AGPL-3.0-only
// CTIDH-1024's field for the ESP32-S3 (Xtensa LX7), in place of fp1024_mont32.c and with its
// contract: fiat_p1024_mul, _square, _add and _sub on elements in the Montgomery domain with
// R = 2^1024, inputs and outputs fully reduced, so every result is the same bytes fiat's is.
// Windowed ABI; the output may alias an input.
//
// Constant time: fixed trip counts, fixed addresses, and no branch on data. Carries and
// borrows are computed with logic and MINU (this core has no SALTU).
//
// Multiply: Montgomery multiplication by product scanning (FIPS: Koc, Acar, Kaliski 1996) over
// 16-bit digits on MAC16, which multiplies signed 16-bit halves into a 40-bit accumulator and can
// load the next operand in the same instruction. a, b and p are recoded into signed digits in
// [-2^15, 2^15): N + 0x8000...8000 has 16-bit halves h_i, and h_i - 2^15 are N's digits, so the
// stored word is (N + 0x8000...8000) xor 0x8000...8000. q's digits are signed too, so the result
// t = (ab + qp)/R lies in (-p, p), and p is added once when t < 0.

#define N 32

    .section .rodata.fp1024_xtensa, "a"
    .align  4
.Lp:    // p, 32 words, least significant first
    .word   0x60e36453, 0xdbe34c54, 0xbc3d344d, 0xa1d81eeb, 0xb8d89fd3, 0x514ba72c, 0xe287f1bd, 0xc2cab6a0
    .word   0x5a313709, 0x642aca4d, 0x31541f40, 0x6b317c54, 0xde81ede5, 0xb97c56d1, 0xd90a2b58, 0x0978dbee
    .word   0x90441c80, 0x7611ad4f, 0x19ec8329, 0xf811d9c4, 0x8ad82d2d, 0x4d6c594a, 0x1cf9386e, 0xf06de247
    .word   0xdb31ad5b, 0x0683cf25, 0x86f21a08, 0x216c22bc, 0x9007ebd7, 0xd89dec87, 0x427012a9, 0x0ece55ed
.Lpsd:  // p in signed 16-bit digits, as the recoding stores them
    .word   0x60e36453, 0xdbe34c54, 0xbc3d344e, 0xa1d81eec, 0xb8d99fd4, 0x514ca72d, 0xe288f1bd, 0xc2cbb6a1
    .word   0x5a31370a, 0x642bca4d, 0x31541f40, 0x6b317c54, 0xde82ede5, 0xb97c56d2, 0xd90a2b59, 0x0979dbef
    .word   0x90441c80, 0x7612ad50, 0x19ed8329, 0xf812d9c4, 0x8ad82d2e, 0x4d6c594b, 0x1cf9386e, 0xf06ee247
    .word   0xdb32ad5c, 0x0684cf26, 0x86f21a08, 0x216c22bd, 0x9008ebd7, 0xd89eec88, 0x427012aa, 0x0ece55ed

// A word of N's signed digits from the word at src+soff, to dst+doff: a7 is the carry between
// words, a15 = 0x80008000.
    .macro  RECODE src, soff, dst, doff
    l32i    a9, \src, \soff
    add     a10, a9, a15
    add     a10, a10, a7
    xor     a10, a10, a15
    or      a9, a9, a10
    srli    a7, a9, 31          // the carry: W's top bit, or the sum's top bit clear
    s32i    a10, \dst, \doff
    .endm

// The same for square, word j of a 4-word group: a's digits to X[2w] and Y[2w+1], p's to Y[2w],
// 0 to X[2w+1]; a6 = &Y[2w] of the group's first word (X[0] is Y[0] + 264 bytes).
    .macro  RECODE2 j
    l32i    a9, a3, 4*\j
    l32i    a11, a5, 4*\j
    add     a10, a9, a15
    add     a10, a10, a7
    xor     a10, a10, a15
    or      a9, a9, a10
    srli    a7, a9, 31
    s32i    a11, a6, 8*\j
    s32i    a10, a6, 8*\j+4
    s32i    a10, a6, 264+8*\j
    s32i    a14, a6, 268+8*\j
    .endm

// One step of an even column 2k: m0 = A_w, m1 = Q_w, m2 = B_(k-w), m3 = P_(k-w) on entry, the
// same for w+1 on exit; a3 walks X up, a4 walks Y down.
    .macro  EVEN_STEP
    mula.dd.ll.lddec m2, a4, m0, m2     // a_2w b_2(k-w); m2 = B_(k-w-1)
    mula.dd.ll.lddec m3, a4, m1, m3     // q_2w p_2(k-w); m3 = P_(k-w-1)
    mula.dd.hh.ldinc m0, a3, m0, m2     // a_2w+1 b_2(k-w-1)+1; m0 = A_(w+1)
    mula.dd.hh.ldinc m1, a3, m1, m3     // q_2w+1 p_2(k-w-1)+1; m1 = Q_(w+1)
    .endm

// One step of an odd column 2k+1: m0 = A_w, m1 = Q_w, m2 = B_(k-w) on entry; m3 is loaded.
    .macro  ODD_STEP
    mula.dd.lh.lddec m3, a4, m0, m2     // a_2w b_2(k-w)+1; m3 = P_(k-w)
    mula.dd.hl.ldinc m0, a3, m0, m2     // a_2w+1 b_2(k-w); m0 = A_(w+1)
    mula.dd.lh.lddec m2, a4, m1, m3     // q_2w p_2(k-w)+1; m2 = B_(k-w-1)
    mula.dd.hl.ldinc m1, a3, m1, m3     // q_2w+1 p_2(k-w); m1 = Q_(w+1)
    .endm

// One step of an odd column 2k+1 scanned downwards, X down and Y up: m0 = Q_w, m1 = A_w,
// m2 = P_(k-w) on entry; m3 is loaded.
    .macro  ODD_STEP_DOWN
    mula.dd.lh.ldinc m3, a4, m0, m2     // q_2w p_2(k-w)+1; m3 = B_(k-w)
    mula.dd.hl.ldinc m2, a4, m0, m2     // q_2w+1 p_2(k-w); m2 = P_(k-w+1)
    mula.dd.lh.lddec m0, a3, m1, m3     // a_2w b_2(k-w)+1; m0 = Q_(w-1)
    mula.dd.hl.lddec m1, a3, m1, m3     // a_2w+1 b_2(k-w); m1 = A_(w-1)
    .endm

// ACC = ACC >> 16 (arithmetic), with SAR = 16; a12 = ACCLO already for SHIFT_LO.
    .macro  SHIFT_LO
    rsr.acchi a13
    src     a12, a13, a12
    srai    a13, a13, 16
    wsr.acclo a12
    wsr.acchi a13
    .endm
    .macro  SHIFT
    rsr.acclo a12
    SHIFT_LO
    .endm

// out[off] += (p[off] & a12) + carry a13, a15 = 1: p has no all-ones word, so p_j + carry cannot
// wrap, and the sum wrapped exactly when it is below out[off].
    .macro  ADDP off
    l32i    a3, a8, \off
    l32i    a4, a11, \off
    and     a4, a4, a12
    add     a4, a4, a13
    add     a5, a3, a4
    minu    a6, a5, a3
    sub     a6, a3, a6
    minu    a13, a6, a15
    s32i    a5, a8, \off
    .endm

    .section .text.fp1024_xtensa, "ax"

// out = a * b / R mod p.  a2 = out, a3 = a, a4 = b.
// Frame (words): Y[-2..65] at sp, Y[v] at sp + 8 + 4v: Y[2v] = p_v, Y[2v+1] = b_v, zero at
// v = -1 and 32; X[0..65] at sp + 272: X[2w] = a_w, X[2w+1] = q_w (q's digits, zero until known).
    .align  4
    .global fiat_p1024_mul
    .type   fiat_p1024_mul, @function
fiat_p1024_mul:
    entry   a1, 560
    movi    a15, 0x80008000
    movi    a14, 0
    // b's and p's signed digits into Y
    movi    a5, .Lpsd
    addi    a6, a1, 8
    movi    a7, 0
    movi    a8, N / 4
    loop    a8, 1f
    RECODE  a4, 0, a6, 4
    l32i    a11, a5, 0
    s32i    a11, a6, 0
    RECODE  a4, 4, a6, 12
    l32i    a11, a5, 4
    s32i    a11, a6, 8
    RECODE  a4, 8, a6, 20
    l32i    a11, a5, 8
    s32i    a11, a6, 16
    RECODE  a4, 12, a6, 28
    l32i    a11, a5, 12
    s32i    a11, a6, 24
    addi    a4, a4, 16
    addi    a5, a5, 16
    addi    a6, a6, 32
1:
    // a's signed digits into X, q zeroed
    addmi   a6, a1, 256
    addi    a6, a6, 16
    movi    a7, 0
    movi    a8, N / 4
    loop    a8, 1f
    RECODE  a3, 0, a6, 0
    s32i    a14, a6, 4
    RECODE  a3, 4, a6, 8
    s32i    a14, a6, 12
    RECODE  a3, 8, a6, 16
    s32i    a14, a6, 20
    RECODE  a3, 12, a6, 24
    s32i    a14, a6, 28
    addi    a3, a3, 16
    addi    a6, a6, 32
1:
    j       fp1024_scan
    .size   fiat_p1024_mul, . - fiat_p1024_mul

// out = a^2 / R mod p: the multiply with a's digits in both X and Y.
    .align  4
    .global fiat_p1024_square
    .type   fiat_p1024_square, @function
fiat_p1024_square:
    entry   a1, 560
    movi    a15, 0x80008000
    movi    a14, 0
    movi    a5, .Lpsd
    addi    a6, a1, 8
    movi    a7, 0
    movi    a8, N / 4
    loop    a8, 1f
    RECODE2 0
    RECODE2 1
    RECODE2 2
    RECODE2 3
    addi    a3, a3, 16
    addi    a5, a5, 16
    addi    a6, a6, 32
1:
    j       fp1024_scan
    .size   fiat_p1024_square, . - fiat_p1024_square

// The scan shared by multiply and square, in fiat_p1024_mul's frame; a14 = 0, a2 = out.
    .align  4
    .type   fp1024_scan, @function
fp1024_scan:
    s32i    a14, a1, 0          // Y[-2], Y[-1], Y[64], Y[65], X[64], X[65]
    s32i    a14, a1, 4
    s32i    a14, a1, 264
    s32i    a14, a1, 268
    s32i    a14, a1, 528
    s32i    a14, a1, 532
    ssai    16
    wsr.acclo a14
    wsr.acchi a14
    addmi   a5, a1, 256
    addi    a5, a5, 12          // &X[0] - 4, the X pointer's start
    addi    a6, a1, 16          // &Y[2k + 2], the Y pointer's start
    addi    a14, a5, 4          // a6 at k = 32
    movi    a7, 1               // k + 1 steps
    movi    a9, 0x8025          // -p^-1 mod 2^16
    movi    a10, 0x6453         // p's lowest digit
    // Columns 2k and 2k+1 for k = 0..31. A step w adds the products of A_w, Q_w with B_(k-w),
    // P_(k-w) (and with B_(k-w-1), P_(k-w-1) in an even column) whose digits land in this column;
    // then q's digit is chosen so the column is 0 mod 2^16, and the column shifted out.
.Llow:
    mov     a3, a5
    mov     a4, a6
    ldinc   m0, a3
    ldinc   m1, a3
    lddec   m2, a4
    lddec   m3, a4
    loop    a7, 1f
    EVEN_STEP
1:
    rsr.acclo a11
    mull    a11, a11, a9
    s16i    a11, a6, 260        // q_2k into X[2k + 1], at a6 + 260
    mula.aa.ll a11, a10
    SHIFT
    mov     a3, a5
    mov     a4, a6
    ldinc   m0, a3
    ldinc   m1, a3
    lddec   m2, a4
    loop    a7, 1f
    ODD_STEP
1:
    rsr.acclo a11
    mull    a11, a11, a9
    s16i    a11, a6, 262
    mula.aa.ll a11, a10
    SHIFT
    addi    a6, a6, 8
    addi    a7, a7, 1
    bne     a6, a14, .Llow
    // Columns 2k and 2k+1 for k = 32..63: steps w = k-32..31 (the first one meets the zero at
    // v = 32), then w = 32 down to k-31, from where the even scan stopped (w = 32 meets the zeros
    // in X[64], X[65]); each column's low 16 bits are the output's digit 2k-64 or 2k-63.
    mov     a8, a2
    movi    a7, 32              // 64 - k steps
.Lhigh:
    mov     a3, a5
    mov     a4, a6
    ldinc   m0, a3
    ldinc   m1, a3
    lddec   m2, a4
    lddec   m3, a4
    loop    a7, 1f
    EVEN_STEP
1:
    rsr.acclo a12
    s16i    a12, a8, 0
    SHIFT_LO
    addi    a3, a3, -4
    addi    a5, a5, 8           // the next column's X start; here it also aligns the loop
    loop    a7, 1f
    ODD_STEP_DOWN
1:
    rsr.acclo a12
    s16i    a12, a8, 2
    SHIFT_LO
    addi    a8, a8, 4
    addi    a7, a7, -1
    bnez    a7, .Lhigh
    // a12 is what is left above the top digit, 0 or -1: the result is negative (above -p) and
    // p is added back.
    movi    a11, .Lp
    movi    a13, 0
    movi    a15, 1
    mov     a8, a2
    movi    a7, N / 4
    loop    a7, 1f
    ADDP    0
    ADDP    4
    ADDP    8
    ADDP    12
    addi    a8, a8, 16
    addi    a11, a11, 16
1:
    retw
    .size   fp1024_scan, . - fp1024_scan

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
