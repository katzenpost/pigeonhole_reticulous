// SPDX-License-Identifier: AGPL-3.0-only
// The instructions each CTIDH-1024 field primitive executes per call on the ESP32-S3's core,
// counted in Espressif's QEMU (README.md). QEMU runs with -icount, so its clock, and with it
// CCOUNT, advances a fixed amount per instruction: a call's CCOUNT ticks over the ticks per
// instruction, set by a block of known length, are the instructions it executed. Every
// interrupt is masked while counting, and the second core is parked, so that only the call's
// own instructions advance the clock.
//
// Prints one line per primitive, which table.py makes into the Markdown table:
//   KQ count <name> <address> <fewest> <most>   (over CHAIN chained calls: equal unless the
//                                                instruction count depends on the data)
//   KQ check <name> <calls> <differing>         (its outputs against its *_ref's)
//   KQ done | KQ fail <why>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp32s3/rom/ets_sys.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "fiat_p1024.h"

// The primitives, each with its *_ref where the build has one, called as f(x, x, y) with
// each result the next x. To add one: declare it and add a row; a unary one (out, a) is
// called through the same pointer type, its y unused.
typedef void (*prim)(uint64_t out[16], const uint64_t a[16], const uint64_t b[16]);
#ifdef KQ_FIELD_REF
void fiat_p1024_mul_ref(uint64_t[16], const uint64_t[16], const uint64_t[16]);
void fiat_p1024_square_ref(uint64_t[16], const uint64_t[16]);
void fiat_p1024_add_ref(uint64_t[16], const uint64_t[16], const uint64_t[16]);
void fiat_p1024_sub_ref(uint64_t[16], const uint64_t[16], const uint64_t[16]);
#define REF(f) (prim) f
#else
#define REF(f) NULL
#endif
static const struct {
    const char* name;
    prim f, ref;
} prims[] = {
    {"field-mul-" KQ_FIELD_NAME, fiat_p1024_mul, REF(fiat_p1024_mul_ref)},
    {"field-square-" KQ_FIELD_NAME, (prim)fiat_p1024_square, REF(fiat_p1024_square_ref)},
    {"field-add-" KQ_FIELD_NAME, fiat_p1024_add, REF(fiat_p1024_add_ref)},
    {"field-sub-" KQ_FIELD_NAME, fiat_p1024_sub, REF(fiat_p1024_sub_ref)},
#ifdef KQ_FIELD_REF
    {"field-mul-fiat-ref", fiat_p1024_mul_ref, NULL},
    {"field-square-fiat-ref", (prim)fiat_p1024_square_ref, NULL},
    {"field-add-fiat-ref", fiat_p1024_add_ref, NULL},
    {"field-sub-fiat-ref", fiat_p1024_sub_ref, NULL},
#endif
};
#define CHAIN 16

// CCOUNT ticks of one call of f(x, x, y), with every interrupt masked.
__attribute__((noinline)) static uint32_t ticks(prim f, uint64_t* x, const uint64_t* y) {
    uint32_t ps, t0, t1;
    __asm__ volatile("rsil %0, 15" : "=r"(ps)::"memory");
    __asm__ volatile("rsr.ccount %0" : "=r"(t0)::"memory");
    f(x, x, y);
    __asm__ volatile("rsr.ccount %0" : "=r"(t1)::"memory");
    __asm__ volatile("wsr.ps %0\n\trsync" ::"r"(ps) : "memory");
    return t1 - t0;
}

// The baseline: a function that only enters and returns (entry, retw.n).
__attribute__((noinline)) static void empty(uint64_t* o, const uint64_t* a, const uint64_t* b) {
    __asm__ volatile("" ::: "memory");
}
#define EMPTY_INSNS 2
// The calibration: blocks of known length. The long one, 2^20 nops in a zero-overhead loop,
// sets the ticks per instruction; the short, odd, straight-line one must then count exact.
#define SLED (2 + (1 << 20))  // movi, loop, then 64 nops 2^14 times
__attribute__((noinline)) static void sled(uint64_t* o, const uint64_t* a, const uint64_t* b) {
    __asm__ volatile("movi a8, 1 << 14\n\tloop a8, 1f\n\t.rept 64\n\tnop\n\t.endr\n1:" ::: "a8", "memory");
}
#define SLED_ODD 1001
__attribute__((noinline)) static void sled_odd(uint64_t* o, const uint64_t* a, const uint64_t* b) {
    __asm__ volatile(".rept 1001\n\tnop\n\t.endr" ::: "memory");
}

// The second core. IDF's single-core start stalls it, which QEMU does not model: it would spin
// in ROM waiting for a start address, and under -icount its instructions advance the clock
// too. Given this one, it waits for an interrupt that never comes.
static IRAM_ATTR void park(void) {
    for (;;) __asm__ volatile("waiti 15");
}

static double per_insn;  // CCOUNT ticks per instruction
static uint32_t base;    // ticks of a call of empty
// Instructions of one call of f, from its entry through its return.
static uint32_t insns(prim f, uint64_t* x, const uint64_t* y) {
    return (uint32_t)((ticks(f, x, y) - base) / per_insn + 0.5) + EMPTY_INSNS;
}

// A fully reduced element: below 2^1019, so below p; xorshift, so every run counts the same.
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static void felem(uint64_t x[16]) {
    for (int i = 0; i < 16; i++) {
        rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
        x[i] = rng;
    }
    x[15] &= (1ull << 59) - 1;
}

void app_main(void) {
    static uint64_t x[16], y[16], u[16];
    ets_set_appcpu_boot_addr((uint32_t)park);
    ticks(sled, x, y);  // time for the other core to get there
    base = ticks(empty, x, y);
    per_insn = (double)(ticks(sled, x, y) - base) / SLED;
    uint32_t odd = insns(sled_odd, x, y);
    if (odd != SLED_ODD + EMPTY_INSNS) {
        printf("KQ fail calibration: %.3f ticks per instruction, a %d-instruction block counted %lu\n", per_insn,
               SLED_ODD + EMPTY_INSNS, (unsigned long)odd);
        esp_restart();
    }

    for (size_t k = 0; k < sizeof prims / sizeof prims[0]; k++) {
        felem(x);
        felem(y);
        memcpy(u, x, sizeof u);
        uint32_t lo = UINT32_MAX, hi = 0, differ = 0;
        for (int i = 0; i < CHAIN; i++) {
            uint32_t n = insns(prims[k].f, x, y);
            lo = n < lo ? n : lo;
            hi = n > hi ? n : hi;
            if (prims[k].ref) {
                prims[k].ref(u, u, y);
                differ += memcmp(x, u, sizeof u) != 0;
                memcpy(u, x, sizeof u);  // the chains go on from the same element
            }
        }
        printf("KQ count %s %p %lu %lu\n", prims[k].name, (void*)prims[k].f, (unsigned long)lo, (unsigned long)hi);
        if (prims[k].ref) printf("KQ check %s %d %lu\n", prims[k].name, CHAIN, (unsigned long)differ);
    }
    printf("KQ done\n");
    fflush(stdout);
    esp_restart();  // QEMU runs with -no-reboot, so this ends the run
}
