// SPDX-License-Identifier: AGPL-3.0-only
// CTIDH-1024's field for 32-bit cores, in place of fiat-crypto's fiat_p1024_mul, _square,
// _add and _sub, with their contract: elements in the Montgomery domain with R = 2^1024,
// inputs and outputs fully reduced (0 <= x < p), so every result is the same bytes fiat's
// would be. fiat's code is straight-line 64-bit arithmetic: on the ESP32-S3 its multiply is
// 190 KB of instructions, twelve times the instruction cache, and so streams from flash on
// every call. This one is a few loops over 32-bit words, 1.3 KB.
//
// Constant time: fixed trip counts, no table lookups, and no compares; every carry and
// borrow is computed with logic (Hacker's Delight 2-13), since on a core without an
// add-with-carry GCC turns `s < a` into a branch.
#include <stdint.h>
#include <string.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "fp1024_mont32: a uint64_t[16] is read as a uint32_t[32], which needs little-endian"
#endif

#define N 32

static const uint32_t P[N] = {
    0x60e36453u, 0xdbe34c54u, 0xbc3d344du, 0xa1d81eebu, 0xb8d89fd3u, 0x514ba72cu, 0xe287f1bdu, 0xc2cab6a0u,
    0x5a313709u, 0x642aca4du, 0x31541f40u, 0x6b317c54u, 0xde81ede5u, 0xb97c56d1u, 0xd90a2b58u, 0x0978dbeeu,
    0x90441c80u, 0x7611ad4fu, 0x19ec8329u, 0xf811d9c4u, 0x8ad82d2du, 0x4d6c594au, 0x1cf9386eu, 0xf06de247u,
    0xdb31ad5bu, 0x0683cf25u, 0x86f21a08u, 0x216c22bcu, 0x9007ebd7u, 0xd89dec87u, 0x427012a9u, 0x0ece55edu,
};
static const uint32_t PINV = 0x60038025u;  // -p^-1 mod 2^32 (the low half of fiat's m')

// The carry out of s = a + b (+ a carry in), and the borrow out of d = a - b (- a borrow in).
static inline uint32_t carry_of(uint32_t a, uint32_t b, uint32_t s) { return ((a & b) | ((a | b) & ~s)) >> 31; }
static inline uint32_t borrow_of(uint32_t a, uint32_t b, uint32_t d) { return ((~a & b) | (~(a ^ b) & d)) >> 31; }
static inline uint32_t mulhi(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a * b) >> 32); }

// out = t - p when that does not borrow, else t; t < 2p.
static void reduce_once(uint32_t out[N], const uint32_t t[N]) {
    uint32_t d[N], br = 0;
    for (int j = 0; j < N; j++) {
        uint32_t x = t[j] - P[j], y = x - br;
        br = borrow_of(t[j], P[j], x) | borrow_of(x, br, y);
        d[j] = y;
    }
    uint32_t keep = 0u - br;  // all ones when t < p
    for (int j = 0; j < N; j++) out[j] = (t[j] & keep) | (d[j] & ~keep);
}

// Montgomery multiplication, CIOS (Koc, Acar, Kaliski 1996): r = a * b / R mod p.
__attribute__((noinline)) static void mont_mul(uint32_t r[N], const uint32_t a[N], const uint32_t b[N]) {
    uint32_t t[N + 2];
    memset(t, 0, sizeof t);
    for (int i = 0; i < N; i++) {
        uint32_t bi = b[i], c = 0;
        for (int j = 0; j < N; j++) {
            uint32_t lo = a[j] * bi, hi = mulhi(a[j], bi);
            uint32_t s = t[j] + lo, c1 = carry_of(t[j], lo, s);
            uint32_t s2 = s + c, c2 = carry_of(s, c, s2);
            t[j] = s2;
            c = hi + c1 + c2;
        }
        uint32_t s = t[N] + c;
        t[N + 1] = carry_of(t[N], c, s);
        t[N] = s;

        // t += m * p, which zeroes its lowest word; then t /= 2^32.
        uint32_t m = t[0] * PINV;
        uint32_t lo = m * P[0], hi = mulhi(m, P[0]);
        c = hi + carry_of(t[0], lo, t[0] + lo);
        for (int j = 1; j < N; j++) {
            lo = m * P[j];
            hi = mulhi(m, P[j]);
            uint32_t s1 = t[j] + lo, c1 = carry_of(t[j], lo, s1);
            uint32_t s2 = s1 + c, c2 = carry_of(s1, c, s2);
            t[j - 1] = s2;
            c = hi + c1 + c2;
        }
        s = t[N] + c;
        uint32_t c1 = carry_of(t[N], c, s);
        t[N - 1] = s;
        t[N] = t[N + 1] + c1;
    }
    // t < 2p < 2^1021: t[N] is 0, and one subtraction of p reduces it.
    reduce_once(r, t);
}

void fiat_p1024_mul(uint64_t out1[16], const uint64_t arg1[16], const uint64_t arg2[16]) {
    uint32_t a[N], b[N], r[N];
    memcpy(a, arg1, sizeof a);
    memcpy(b, arg2, sizeof b);
    mont_mul(r, a, b);
    memcpy(out1, r, sizeof r);
}

void fiat_p1024_square(uint64_t out1[16], const uint64_t arg1[16]) {
    uint32_t a[N], r[N];
    memcpy(a, arg1, sizeof a);
    mont_mul(r, a, a);
    memcpy(out1, r, sizeof r);
}

void fiat_p1024_add(uint64_t out1[16], const uint64_t arg1[16], const uint64_t arg2[16]) {
    uint32_t a[N], b[N], s[N], c = 0;
    memcpy(a, arg1, sizeof a);
    memcpy(b, arg2, sizeof b);
    for (int j = 0; j < N; j++) {  // a + b < 2p < 2^1021: no carry out of the top word
        uint32_t x = a[j] + b[j], y = x + c;
        c = carry_of(a[j], b[j], x) | carry_of(x, c, y);
        s[j] = y;
    }
    reduce_once(s, s);
    memcpy(out1, s, sizeof s);
}

void fiat_p1024_sub(uint64_t out1[16], const uint64_t arg1[16], const uint64_t arg2[16]) {
    uint32_t a[N], b[N], d[N], br = 0;
    memcpy(a, arg1, sizeof a);
    memcpy(b, arg2, sizeof b);
    for (int j = 0; j < N; j++) {
        uint32_t x = a[j] - b[j], y = x - br;
        br = borrow_of(a[j], b[j], x) | borrow_of(x, br, y);
        d[j] = y;
    }
    // a < b: add p back.
    uint32_t mask = 0u - br, c = 0;
    for (int j = 0; j < N; j++) {
        uint32_t pj = P[j] & mask, x = d[j] + pj, y = x + c;
        c = carry_of(d[j], pj, x) | carry_of(x, c, y);
        d[j] = y;
    }
    memcpy(out1, d, sizeof d);
}
