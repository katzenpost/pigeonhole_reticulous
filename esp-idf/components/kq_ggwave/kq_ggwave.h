// SPDX-License-Identifier: AGPL-3.0-only
// Voucher codes by sound through ggwave, as docs/sound-vouchers.md sets out: the payload in
// ggwave's variable-length mode with GGWAVE_PROTOCOL_AUDIBLE_FAST, as 16 kHz PCM16. The C++ face of
// the Rust in components/kq_rust (src/ggwave.rs), which has the whole of it.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "kq_rust.h"

namespace kqgg {

constexpr int kRate = 16000;
constexpr int kPayload = 32;
constexpr int kVolume = 25;  // ggwave's volume, 0-100

// The payload's sound at 16 kHz; empty if ggwave refuses it or there is no memory.
std::vector<int16_t> encode(const uint8_t* p, size_t n, int volume = kVolume);

// The receiver, fed the microphone as it comes, in blocks of any size.
class Rx {
public:
    Rx();
    ~Rx();
    Rx(const Rx&) = delete;
    Rx& operator=(const Rx&) = delete;
    bool ok() const { return m != nullptr; }  // false if out of memory
    // Takes n samples; the length of a payload they completed, copied into out[cap], else 0.
    int push(const int16_t* x, size_t n, uint8_t* out, size_t cap);
    int failed = 0;  // transmissions heard whose payload did not decode

private:
    KqGgRx* m;
};

// A polyphase resampler by up/down through one low-pass at 48 kHz (7.4 kHz): 16 <-> 24 kHz,
// and 16 <-> 48 kHz for the tests.
class Resampler {
public:
    Resampler(int up, int down);
    ~Resampler();
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;
    // n samples in; the outputs they complete, at most n * up / down + 1, into y.
    size_t run(const float* x, size_t n, float* y);

private:
    KqGgResampler* r_;
};

}  // namespace kqgg
