// SPDX-License-Identifier: AGPL-3.0-only
// CTIDH-1024's field (components/kq_ctidh/fp1024_mont32.c) against fiat-crypto's, kept beside
// it as fiat_p1024_*_ref: every result the same bytes, for random elements below p, the edge
// values, and long chains of dependent operations.
#include <sodium.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "vectors.h"

extern "C" {
void fiat_p1024_mul(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_square(uint64_t o[16], const uint64_t a[16]);
void fiat_p1024_add(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_sub(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_mul_ref(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_square_ref(uint64_t o[16], const uint64_t a[16]);
void fiat_p1024_add_ref(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_sub_ref(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
}

struct Fe { uint64_t w[16]; };

static const Fe kP = {{0xdbe34c5460e36453, 0xa1d81eebbc3d344d, 0x514ba72cb8d89fd3, 0xc2cab6a0e287f1bd,
                       0x642aca4d5a313709, 0x6b317c5431541f40, 0xb97c56d1de81ede5, 0x0978dbeed90a2b58,
                       0x7611ad4f90441c80, 0xf811d9c419ec8329, 0x4d6c594a8ad82d2d, 0xf06de2471cf9386e,
                       0x0683cf25db31ad5b, 0x216c22bc86f21a08, 0xd89dec879007ebd7, 0x0ece55ed427012a9}};

static bool below_p(const Fe& x) {
    for (int i = 15; i >= 0; i--)
        if (x.w[i] != kP.w[i]) return x.w[i] < kP.w[i];
    return false;
}

static Fe random_fe() {
    for (;;) {
        Fe x;
        randombytes_buf(x.w, sizeof x.w);
        x.w[15] &= 0x0fffffffffffffffull;  // below 2^1020, then rejected unless below p
        if (below_p(x)) return x;
    }
}

static Fe minus(const Fe& a, uint64_t k) {  // a - k, for small k and a >= k
    Fe r = a;
    for (int i = 0; i < 16 && k; i++) { uint64_t o = r.w[i]; r.w[i] -= k; k = o < k ? 1 : 0; }
    return r;
}

static bool same(const Fe& a, const Fe& b) { return memcmp(a.w, b.w, sizeof a.w) == 0; }

int main() {
    if (sodium_init() < 0) return 2;
    Vec v{"field", "", {}};

    // The edge values: 0, 1, 2, p-1, p-2, R mod p (1 doubled 1024 times by fiat's add), every
    // word set below p's top word, and p's top word alone.
    Fe zero{}, one{}, two{};
    one.w[0] = 1;
    two.w[0] = 2;
    Fe r = one;
    for (int i = 0; i < 1024; i++) fiat_p1024_add_ref(r.w, r.w, r.w);
    Fe ones;
    for (int i = 0; i < 15; i++) ones.w[i] = ~0ull;
    ones.w[15] = kP.w[15] - 1;
    Fe high{};
    high.w[15] = kP.w[15];
    std::vector<Fe> edge = {zero, one, two, minus(kP, 1), minus(kP, 2), r, ones, high};
    for (auto& e : edge) check(below_p(e), v, "edge value below p");

    size_t bad = 0, n = 0;
    auto cmp = [&](const Fe& a, const Fe& b) {
        Fe x, y;
        fiat_p1024_mul(x.w, a.w, b.w);
        fiat_p1024_mul_ref(y.w, a.w, b.w);
        bad += !same(x, y);
        fiat_p1024_square(x.w, a.w);
        fiat_p1024_square_ref(y.w, a.w);
        bad += !same(x, y);
        fiat_p1024_add(x.w, a.w, b.w);
        fiat_p1024_add_ref(y.w, a.w, b.w);
        bad += !same(x, y);
        fiat_p1024_sub(x.w, a.w, b.w);
        fiat_p1024_sub_ref(y.w, a.w, b.w);
        bad += !same(x, y);
        n += 4;
    };
    for (auto& a : edge)
        for (auto& b : edge) cmp(a, b);
    check(bad == 0, v, "edge values");
    size_t edge_n = n;

    bad = 0;
    for (int i = 0; i < 500000; i++) {  // 10^6 random elements
        Fe a = random_fe(), b = random_fe();
        cmp(a, b);
        if (i % 1000 == 0)
            for (auto& e : edge) { cmp(a, e); cmp(e, a); }
    }
    check(bad == 0, v, "random elements");

    // R mod p is the Montgomery form of 1: a * R / R = a, without the reference.
    bad = 0;
    for (int i = 0; i < 1000; i++) {
        Fe a = random_fe(), x;
        fiat_p1024_mul(x.w, a.w, r.w);
        bad += !same(x, a);
    }
    check(bad == 0, v, "a times R mod p is a");

    // Chains: each result fed to the next, as CTIDH does, mixing the four operations.
    Fe x = random_fe(), y = x, k = random_fe();
    bad = 0;
    for (int i = 0; i < 10000; i++) {
        switch (i % 4) {
            case 0: fiat_p1024_mul(x.w, x.w, k.w); fiat_p1024_mul_ref(y.w, y.w, k.w); break;
            case 1: fiat_p1024_square(x.w, x.w); fiat_p1024_square_ref(y.w, y.w); break;
            case 2: fiat_p1024_add(x.w, x.w, k.w); fiat_p1024_add_ref(y.w, y.w, k.w); break;
            case 3: fiat_p1024_sub(x.w, k.w, x.w); fiat_p1024_sub_ref(y.w, k.w, y.w); break;
        }
        bad += !same(x, y);
        if (!below_p(x)) bad++;
    }
    check(bad == 0, v, "chains of 10000 dependent operations");
    printf("field: %zu edge and %zu random comparisons, 10000 chained\n", edge_n, n - edge_n);
    return finish("field");
}
