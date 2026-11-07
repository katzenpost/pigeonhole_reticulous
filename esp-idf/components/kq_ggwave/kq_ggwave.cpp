// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_ggwave.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include "ggwave/ggwave.h"

namespace kqgg {
namespace {

constexpr float kInner = 24000;  // ggwave's rate here: 512-sample frames are its 46.875 Hz bins
constexpr int kFrame = 512;
constexpr auto kProtocol = GGWAVE_PROTOCOL_AUDIBLE_FAST;
constexpr int kTaps = 144;  // at 48 kHz: flat to 6.4 kHz, the protocol's top tone; -70 dB past 8.3 kHz
constexpr int kBlock = 320;

struct Lowpass {
    float h[kTaps];
    Lowpass() {
        const double fc = 7400.0 / 48000, mid = (kTaps - 1) / 2.0;
        double sum = 0;
        for (int k = 0; k < kTaps; k++) {
            double t = k - mid, w = 2 * M_PI * k / (kTaps - 1);
            double s = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
            h[k] = (float)(s * (0.42 - 0.5 * cos(w) + 0.08 * cos(2 * w)));  // Blackman
            sum += h[k];
        }
        for (float& c : h) c = (float)(c / sum);
    }
};
const Lowpass& lowpass() {
    static const Lowpass lp;
    return lp;
}

GGWave::Parameters params(int mode) {
    GGWave::setLogFile(nullptr);
    GGWave::Protocols::tx().only(kProtocol);
    GGWave::Protocols::rx().only(kProtocol);
    GGWave::Parameters p = GGWave::getDefaultParameters();
    p.sampleRateInp = p.sampleRateOut = p.sampleRate = kInner;
    p.samplesPerFrame = kFrame;
    p.sampleFormatInp = p.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_F32;
    p.operatingMode = mode;
    return p;
}

}  // namespace

size_t Resampler::run(const float* x, size_t n, float* y) {
    static_assert(kHist > kTaps, "an output at up 1 reaches back kTaps inputs");
    const float* h = lowpass().h;
    size_t m = 0;
    for (size_t j = 0; j < n; j++) {
        hist_[in_ % kHist] = x[j];
        in_++;
        for (; next_ / up_ < in_; next_ += down_) {
            int64_t i = next_ / up_;
            float acc = 0;
            for (int64_t k = next_ - i * up_; k < kTaps && i >= 0; k += up_, i--) acc += h[k] * hist_[i % kHist];
            y[m++] = acc * up_;
        }
    }
    return m;
}

std::vector<int16_t> encode(const uint8_t* p, size_t n, int volume) try {
    GGWave::Parameters par = params(GGWAVE_OPERATING_MODE_TX);
    par.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;  // its float copy would be 0.5 MB more
    GGWave gg;
    if (!gg.prepare(par) || !gg.init((int)n, (const char*)p, kProtocol, volume)) return {};
    uint32_t bytes = gg.encode();
    if (!bytes) return {};
    const int16_t* w = (const int16_t*)gg.txWaveform();
    size_t nw = bytes / 2, total = nw + 64;  // and zeros, for the filter's tail
    Resampler down(2, 3);
    std::vector<int16_t> out(total * 2 / 3 + 1);
    float x[kBlock], y[kBlock];
    size_t m = 0;
    for (size_t at = 0; at < total; at += kBlock) {
        size_t b = std::min<size_t>(kBlock, total - at);
        for (size_t i = 0; i < b; i++) x[i] = at + i < nw ? w[at + i] / 32768.0f : 0;
        size_t ny = down.run(x, b, y);
        for (size_t i = 0; i < ny; i++) out[m++] = (int16_t)std::lrint(std::clamp(y[i] * 32768.0f, -32768.0f, 32767.0f));
    }
    out.resize(m);
    return out;
} catch (const std::bad_alloc&) {
    return {};
}

struct Rx::Impl {
    GGWave gg;
    Resampler up{3, 2};
    float x[kBlock], y[kBlock * 3 / 2 + 1], frame[kFrame];
    int nf = 0;
};

Rx::Rx() : m(new (std::nothrow) Impl) {
    if (m && !m->gg.prepare(params(GGWAVE_OPERATING_MODE_RX))) {
        delete m;
        m = nullptr;
    }
}

Rx::~Rx() { delete m; }

int Rx::push(const int16_t* x, size_t n, uint8_t* out, size_t cap) {
    if (!m) return 0;
    int got = 0;
    for (size_t at = 0; at < n; at += kBlock) {
        size_t b = std::min<size_t>(kBlock, n - at);
        for (size_t i = 0; i < b; i++) m->x[i] = x[at + i] / 32768.0f;
        size_t ny = m->up.run(m->x, b, m->y);
        for (size_t i = 0; i < ny; i++) {
            m->frame[m->nf++] = m->y[i];
            if (m->nf < kFrame) continue;
            m->nf = 0;
            m->gg.decode(m->frame, sizeof m->frame);  // whole frames only: it drops part of one
            GGWave::TxRxData d;
            int len = m->gg.rxTakeData(d);
            if (len < 0) failed++;
            if (len > 0 && (size_t)len <= cap && !got) {
                memcpy(out, d.data(), len);
                got = len;
            }
        }
    }
    return got;
}

}  // namespace kqgg
