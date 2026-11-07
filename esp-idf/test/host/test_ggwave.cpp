// SPDX-License-Identifier: AGPL-3.0-only
// Vouchers by sound through ggwave (components/kq_ggwave, docs/sound-vouchers.md): a clean
// loopback and its level, both ways against ggwave at its 48 kHz defaults through its C API, the
// WAV fixtures, the receiver through a simulated room at several SNRs, noise, and the check code.
//   test_ggwave <vectors.txt>                                  the checks
//   test_ggwave <vectors.txt> sweep <n> [room|stress|dry] [drift]   FER tables, n trials a point
//   test_ggwave <vectors.txt> fixtures <dir>                   writes the WAV fixtures
//   test_ggwave <vectors.txt> decode <wav>                     decodes a recording
//   test_ggwave <vectors.txt> room <in.wav> <out.wav> <snr_dB> [drift]   plays a recording across the room
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "ggwave/ggwave.h"
#include "kq1_engine.h"
#include "kq_ggwave.h"
#include "room.h"
#include "vectors.h"

namespace {

constexpr size_t kMax = 16000 * 16;
int16_t g_rx[kMax];

kq::Bytes randomVoucher() {
    kq::Bytes v(32);
    for (auto& b : v) b = (uint8_t)room_rand();
    return v;
}

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// Streams x through a receiver 20 ms at a time, as the device does; at is where it was heard.
int listen(const int16_t* x, size_t n, uint8_t* out, size_t* at = nullptr, int* failed = nullptr) {
    kqgg::Rx rx;
    int got = 0;
    size_t i = 0;
    for (; i < n && !got; i += 320) got = rx.push(x + i, std::min<size_t>(320, n - i), out, 32);
    if (at) *at = i;
    if (failed) *failed = rx.failed;
    return got;
}

struct Point {
    int fail = 0, wrong = 0, ok = 0, failed = 0;
    double ms = 0, late_max = -1e9;
};

// One voucher through the room at snr after 0.5-3 s of noise, drift alternately + and -.
Point trial(const room_chan& c, double snr, double drift, int t, Point p) {
    kq::Bytes v = randomVoucher();
    auto tx = kqgg::encode(v.data(), v.size());
    double d = t & 1 ? drift : -drift, t0;
    size_t lead = (size_t)(16000 * (0.5 + 2.5 * room_uniform()));
    size_t n = room_channel(tx.data(), tx.size(), lead, 6400, &c, d, snr, g_rx, kMax, &t0);
    uint8_t out[32];
    size_t at;
    int failed;
    auto clk = std::chrono::steady_clock::now();
    int got = listen(g_rx, n, out, &at, &failed);
    p.ms += msSince(clk);
    p.failed += failed;
    if (got != 32) p.fail++;
    else if (memcmp(out, v.data(), 32)) p.wrong++;
    else {
        p.ok++;
        p.late_max = std::max(p.late_max, (at - (t0 + tx.size() * (1 + d))) / 16.0);
    }
    return p;
}

Point run(const room_chan& c, double snr, double drift, int n) {
    Point p;
    for (int t = 0; t < n; t++) p = trial(c, snr, drift, t, p);
    return p;
}

void sweep(int n, const room_chan& c, double drift) {
    static const double room[] = {-6, -3, 0, 3, 6, 10, 15, 20}, few[] = {0, 6, 12, 20, 30};
    const double* snrs = &c == &ROOM_STRESS ? few : room;
    int ns = &c == &ROOM_STRESS ? 5 : 8;
    printf("channel %s: rt60 %.2f s, DRR %.0f dB, early reflections up to %.2f, drift +-%.2f%%, %d trials a point\n",
           c.name, c.rt60, c.drr_db, c.early, drift * 100, n);
    printf("%6s %7s %6s %9s %8s %9s\n", "snr_dB", "FER", "wrong", "undecoded", "ms_avg", "late_max");
    for (int s = 0; s < ns; s++) {
        Point p = run(c, snrs[s], drift, n);
        printf("%6.1f %7.3f %6d %9d %8.1f %9.0f\n", snrs[s], (double)(p.fail + p.wrong) / n, p.wrong, p.failed,
               p.ms / n, p.late_max);
        fflush(stdout);
    }
}

const char* kFixtures[] = {"tx", "room-10dB", "room-0dB"};

void fixtures(const std::string& dir, const kq::Bytes& v) {
    auto tx = kqgg::encode(v.data(), v.size());
    memset(g_rx, 0, sizeof g_rx);  // as a device plays it: 0.2 s of quiet first, 0.5 s after
    memcpy(g_rx + 3200, tx.data(), tx.size() * 2);
    wav_write((dir + "/tx.wav").c_str(), g_rx, 3200 + tx.size() + 8000);
    room_seed(7);
    double t0;
    size_t n = room_channel(tx.data(), tx.size(), 4000, 6400, &ROOM_TABLE, 0.0005, 10, g_rx, kMax, &t0);
    wav_write((dir + "/room-10dB.wav").c_str(), g_rx, n);
    n = room_channel(tx.data(), tx.size(), 7000, 6400, &ROOM_TABLE, -0.0005, 0, g_rx, kMax, &t0);
    wav_write((dir + "/room-0dB.wav").c_str(), g_rx, n);
}

int decodeFile(const char* path) {
    size_t n = wav_read(path, g_rx, kMax);
    if (!n) { fprintf(stderr, "%s: not a 16 kHz mono PCM16 WAV\n", path); return 2; }
    uint8_t out[32];
    size_t at;
    int failed;
    if (listen(g_rx, n, out, &at, &failed) != 32) {
        printf("%s: no voucher (%zu samples, %d heard but undecoded)\n", path, n, failed);
        return 1;
    }
    std::string code = kq1::voucher_code(kq::Bytes(out, out + 32));
    printf("%s: %s, check %s, heard by %.2f s\n", path, code.c_str(), kq1::voucher_check(code).c_str(), at / 16000.0);
    return 0;
}

// ggwave as another program would run it: its C API and default parameters (48 kHz, 1024-sample
// frames, float), receiving Audible Fast only.
ggwave_Instance defaultGgwave() {
    for (int i = 0; i < GGWAVE_PROTOCOL_COUNT; i++) {
        ggwave_rxToggleProtocol((ggwave_ProtocolId)i, i == GGWAVE_PROTOCOL_AUDIBLE_FAST);
        ggwave_txToggleProtocol((ggwave_ProtocolId)i, i == GGWAVE_PROTOCOL_AUDIBLE_FAST);
    }
    return ggwave_init(ggwave_getDefaultParameters());
}

}  // namespace

int main(int argc, char** argv) {
    Vec vec{"ggwave", "sound", {}};
    kq::Bytes voucher;
    for (auto& v : load(argv[1], "voucher/voucher_vectors_v2.json"))
        if (v.name == "mint") voucher = v.hex("voucher");
    std::string mode = argc > 2 ? argv[2] : "";
    if (mode == "sweep" && argc > 3) {
        std::string ch = argc > 4 ? argv[4] : "room";
        sweep(atoi(argv[3]), ch == "stress" ? ROOM_STRESS : ch == "dry" ? ROOM_DRY : ROOM_TABLE,
              argc > 5 ? atof(argv[5]) : 0.0005);
        return 0;
    }
    if (mode == "fixtures" && argc > 3) { fixtures(argv[3], voucher); return 0; }
    if (mode == "decode" && argc > 3) return decodeFile(argv[3]);
    if (mode == "room" && argc > 5) {
        static int16_t in[kMax];
        size_t n = wav_read(argv[3], in, kMax / 2);
        double t0;
        size_t m = n ? room_channel(in, n, 1600, 6400, &ROOM_TABLE, argc > 6 ? atof(argv[6]) : 0.0005, atof(argv[5]), g_rx,
                                    kMax, &t0)
                     : 0;
        return m && wav_write(argv[4], g_rx, m) ? 0 : 2;
    }

    // A clean voucher between silences: its length, its level, heard.
    auto tx = kqgg::encode(voucher.data(), voucher.size());
    {
        double e = 0;
        int peak = 0;
        for (int16_t s : tx) { e += (double)s * s; peak = std::max(peak, abs(s)); }
        double dbfs = 20 * log10(sqrt(e / tx.size()) / 32768);
        printf("clip: %zu samples (%.2f s), %.1f dBFS RMS, peak %d\n", tx.size(), tx.size() / 16000.0, dbfs, peak);
        // 16 marker frames, 16 Txs of 6 frames for 3 + 32 + 12 bytes, 16 marker frames; 512/24000 s each.
        check(fabs(tx.size() - 128 * 512 * 2 / 3.0) < 64, vec, "a voucher is 128 frames, 2.73 s");
        check(peak <= 32768 * 0.25 * 1.02, vec, "volume 25 peaks at a quarter of full scale");
        memset(g_rx, 0, sizeof g_rx);
        memcpy(g_rx + 3200, tx.data(), tx.size() * 2);
        uint8_t out[32];
        size_t at;
        check(listen(g_rx, 3200 + tx.size() + 8000, out, &at) == 32 && !memcmp(out, voucher.data(), 32), vec,
              "clean loopback");
        printf("clean loopback: heard %+.0f ms from the sound's end\n", (at - 3200.0 - tx.size()) / 16);
    }

    // Both ways against ggwave as another program runs it.
    {
        ggwave_Instance gg = defaultGgwave();
        check(gg >= 0, vec, "ggwave at its defaults");
        // Ours, heard at 48 kHz in 1024-sample frames.
        kqgg::Resampler up(3, 1);
        std::vector<float> x(tx.size() + 16000), y(x.size() * 3 + 2048);
        for (size_t i = 0; i < tx.size(); i++) x[3200 + i] = tx[i] / 32768.0f;
        size_t ny = up.run(x.data(), x.size(), y.data());
        char got[256];
        int len = 0;
        for (size_t at = 0; at + 1024 <= ny && len <= 0; at += 1024)
            len = ggwave_ndecode(gg, y.data() + at, 1024 * sizeof(float), got, sizeof got);
        check(len == 32 && !memcmp(got, voucher.data(), 32), vec, "ggwave at 48 kHz hears our voucher");
        // Theirs, at volume 25, heard by ours at 16 kHz.
        int nb = ggwave_encode(gg, voucher.data(), 32, GGWAVE_PROTOCOL_AUDIBLE_FAST, kqgg::kVolume, nullptr, 1);
        std::vector<float> w(nb / sizeof(float));
        ggwave_encode(gg, voucher.data(), 32, GGWAVE_PROTOCOL_AUDIBLE_FAST, kqgg::kVolume, w.data(), 0);
        kqgg::Resampler down(1, 3);
        std::vector<float> d(w.size() / 3 + 2);
        size_t nd = down.run(w.data(), w.size(), d.data());
        std::vector<int16_t> pcm(nd + 16000);
        for (size_t i = 0; i < nd; i++) pcm[3200 + i] = (int16_t)lrintf(d[i] * 32767);
        uint8_t out[32];
        check(listen(pcm.data(), pcm.size(), out) == 32 && !memcmp(out, voucher.data(), 32), vec,
              "ours hears ggwave's 48 kHz voucher");
        printf("ggwave at 48 kHz: %zu samples (%.2f s) for the voucher, ours %.2f s\n", w.size(), w.size() / 48000.0,
               tx.size() / 16000.0);
        ggwave_free(gg);
    }

    // The fixtures, made by `fixtures`: the vector's voucher, clean and through the room.
    for (const char* f : kFixtures) {
        std::string path = std::string("vectors/ggwave/") + f + ".wav";
        size_t n = wav_read(path.c_str(), g_rx, kMax);
        uint8_t out[32];
        check(n && listen(g_rx, n, out) == 32 && !memcmp(out, voucher.data(), 32), vec, (path + " decodes").c_str());
    }

    // The room, drift +-0.05%.
    room_seed(1);
    for (double snr : {0.0, 3.0, 10.0, 20.0}) {
        Point p = run(ROOM_TABLE, snr, 0.0005, 12);
        char what[96];
        snprintf(what, sizeof what, "room at %.0f dB SNR: %d of 12 lost", snr, p.fail + p.wrong);
        check(p.wrong == 0 && p.fail <= (snr < 3 ? 2 : 0), vec, what);
        printf("%s, %d heard but undecoded, %.0f ms a voucher, heard %+.0f ms from its end at the latest\n", what, p.failed,
               p.ms / 12, p.late_max);
    }

    // Noise alone, as long as a listen lasts, four times: nothing heard.
    {
        int heard = 0, failed = 0;
        for (int i = 0; i < 4; i++) {
            room_noise(0, g_rx, 16000 * 15);
            uint8_t out[32];
            int f;
            heard += listen(g_rx, 16000 * 15, out, nullptr, &f) != 0;
            failed += f;
        }
        check(heard == 0, vec, "no voucher in 60 s of noise");
        printf("noise: 60 s, %d starts heard but undecoded\n", failed);
    }

    // Two devices show the same digits for one voucher; the vector's, from Python's hashlib.
    std::string code = kq1::voucher_code(voucher);
    check(kq1::voucher_check(code) == "1939", vec, "check code of the vector's voucher");
    check(kq1::voucher_check("AAAA").empty() && kq1::voucher_check("not a code").empty(), vec,
          "no check code for what is not a voucher code");
    return finish("ggwave");
}
