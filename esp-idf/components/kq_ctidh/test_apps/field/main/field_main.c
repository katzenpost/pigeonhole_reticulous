// SPDX-License-Identifier: AGPL-3.0-only
// fp1024_mont32.c against fiat-crypto's field (fiat_p1024_*_ref) on the ESP32-S3, bit for bit:
// edge values, random elements below p, and chains of dependent operations.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

void fiat_p1024_mul(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_square(uint64_t o[16], const uint64_t a[16]);
void fiat_p1024_add(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_sub(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_mul_ref(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_square_ref(uint64_t o[16], const uint64_t a[16]);
void fiat_p1024_add_ref(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_sub_ref(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);

typedef struct { uint64_t w[16]; } fe;

static const fe P = {{0xdbe34c5460e36453, 0xa1d81eebbc3d344d, 0x514ba72cb8d89fd3, 0xc2cab6a0e287f1bd,
                      0x642aca4d5a313709, 0x6b317c5431541f40, 0xb97c56d1de81ede5, 0x0978dbeed90a2b58,
                      0x7611ad4f90441c80, 0xf811d9c419ec8329, 0x4d6c594a8ad82d2d, 0xf06de2471cf9386e,
                      0x0683cf25db31ad5b, 0x216c22bc86f21a08, 0xd89dec879007ebd7, 0x0ece55ed427012a9}};

static int below_p(const fe *x) {
    for (int i = 15; i >= 0; i--)
        if (x->w[i] != P.w[i]) return x->w[i] < P.w[i];
    return 0;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;  // fixed seed: the same elements on every run
static uint64_t next(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return rng;
}
static fe random_fe(void) {
    for (;;) {
        fe x;
        for (int i = 0; i < 16; i++) x.w[i] = next();
        x.w[15] &= 0x0fffffffffffffffull;
        if (below_p(&x)) return x;
    }
}

static unsigned bad, n;
static void cmp(const fe *a, const fe *b) {
    fe x, y;
    fiat_p1024_mul(x.w, a->w, b->w), fiat_p1024_mul_ref(y.w, a->w, b->w), bad += !!memcmp(&x, &y, sizeof x);
    fiat_p1024_square(x.w, a->w), fiat_p1024_square_ref(y.w, a->w), bad += !!memcmp(&x, &y, sizeof x);
    fiat_p1024_add(x.w, a->w, b->w), fiat_p1024_add_ref(y.w, a->w, b->w), bad += !!memcmp(&x, &y, sizeof x);
    fiat_p1024_sub(x.w, a->w, b->w), fiat_p1024_sub_ref(y.w, a->w, b->w), bad += !!memcmp(&x, &y, sizeof x);
    n += 4;
}

void app_main(void) {
    fe edge[7] = {{{0}}, {{1}}, {{2}}, P, P, P, {{0}}};
    edge[3].w[0] -= 1, edge[4].w[0] -= 2;  // p-1, p-2 (p's low word is odd and above 2)
    for (int i = 0; i < 15; i++) edge[5].w[i] = ~0ull;
    edge[5].w[15] = P.w[15] - 1;  // every word below the top one set
    edge[6].w[15] = P.w[15];      // p's top word alone
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 7; j++) cmp(&edge[i], &edge[j]);
    printf("field: edge values: %u comparisons, %u differ\n", n, bad);
    unsigned total_bad = bad;
    bad = n = 0;
    for (int i = 0; i < 2000; i++) {
        fe a = random_fe(), b = random_fe();
        cmp(&a, &b);
    }
    printf("field: random elements: %u comparisons, %u differ\n", n, bad);
    total_bad += bad;
    bad = 0;
    fe x = random_fe(), y = x, k = random_fe();
    for (int i = 0; i < 2000; i++) {
        switch (i % 4) {
            case 0: fiat_p1024_mul(x.w, x.w, k.w), fiat_p1024_mul_ref(y.w, y.w, k.w); break;
            case 1: fiat_p1024_square(x.w, x.w), fiat_p1024_square_ref(y.w, y.w); break;
            case 2: fiat_p1024_add(x.w, x.w, k.w), fiat_p1024_add_ref(y.w, y.w, k.w); break;
            case 3: fiat_p1024_sub(x.w, k.w, x.w), fiat_p1024_sub_ref(y.w, k.w, y.w); break;
        }
        bad += memcmp(&x, &y, sizeof x) != 0 || !below_p(&x);
    }
    printf("field: chain of 2000 dependent operations: %u differ\n", bad);
    total_bad += bad;
    printf("field: %s (%lld ms)\n", total_bad ? "FAIL" : "PASS", (esp_timer_get_time() - t0) / 1000);
}
