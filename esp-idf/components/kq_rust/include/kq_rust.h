// SPDX-License-Identifier: AGPL-3.0-only
// The C ABI of reticulous-core (components/kq_rust), the firmware's one Rust library.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ggwave, src/ggwave.rs; kq_ggwave.h is its C++ face.
typedef struct KqGgRx KqGgRx;
typedef struct KqGgResampler KqGgResampler;

// The payload's sound at 16 kHz, *len samples, freed with kq_gg_free; NULL if ggwave refuses it.
int16_t* kq_gg_encode(const uint8_t* p, size_t n, int volume, size_t* len);
void kq_gg_free(int16_t* w, size_t len);

// NULL if there is not the memory to listen.
KqGgRx* kq_gg_rx_new(void);
void kq_gg_rx_free(KqGgRx* rx);
// Takes n samples; the length of a payload they completed, copied into out[cap], else 0. *failed
// counts the transmissions heard whose payload did not decode.
int kq_gg_rx_push(KqGgRx* rx, const int16_t* x, size_t n, uint8_t* out, size_t cap, int* failed);

KqGgResampler* kq_gg_resampler_new(int up, int down);
void kq_gg_resampler_free(KqGgResampler* r);
// n samples in; the outputs they complete, at most n * up / down + 1, into y.
size_t kq_gg_resampler_run(KqGgResampler* r, const float* x, size_t n, float* y);

#ifdef __cplusplus
}
#endif
