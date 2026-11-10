// SPDX-License-Identifier: AGPL-3.0-only
// kq_ggwave.h over the Rust (components/kq_rust, src/ggwave.rs).
#include "kq_ggwave.h"

#include <new>

namespace kqgg {

std::vector<int16_t> encode(const uint8_t* p, size_t n, int volume) {
    size_t len = 0;
    int16_t* w = kq_gg_encode(p, n, volume, &len);
    std::vector<int16_t> out;
    try {
        out.assign(w, w + len);
    } catch (const std::bad_alloc&) {
    }
    kq_gg_free(w, len);
    return out;
}

Rx::Rx() : m(kq_gg_rx_new()) {}

Rx::~Rx() { kq_gg_rx_free(m); }

int Rx::push(const int16_t* x, size_t n, uint8_t* out, size_t cap) { return kq_gg_rx_push(m, x, n, out, cap, &failed); }

Resampler::Resampler(int up, int down) : r_(kq_gg_resampler_new(up, down)) {}

Resampler::~Resampler() { kq_gg_resampler_free(r_); }

size_t Resampler::run(const float* x, size_t n, float* y) { return kq_gg_resampler_run(r_, x, n, y); }

}  // namespace kqgg
