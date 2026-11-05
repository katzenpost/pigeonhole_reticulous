// SPDX-License-Identifier: AGPL-3.0-only
// The voucher modem (components/kq_fskv): RS and CRC, the FFT, a clean loopback, the WAV fixtures,
// the blind decoder and the streaming receiver through a simulated room at several SNRs, and the
// check code.
//   test_fskv <vectors.txt>                     the checks
//   test_fskv <vectors.txt> sweep <n> [room|stress|dry]   FER tables, n trials a point
//   test_fskv <vectors.txt> fixtures <dir>      writes the WAV fixtures
//   test_fskv <vectors.txt> decode <wav>        decodes a recording
//   test_fskv <vectors.txt> room <in.wav> <out.wav> <snr_dB> [drift]   plays a recording across the room
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "fskv.h"
#include "fskv_room.h"
#include "kq1_engine.h"
#include "vectors.h"

namespace {

constexpr size_t kMax = 16000 * 16;
int16_t g_rx[kMax], g_tx[FSKV_SAMPLES];
constexpr float kAmp = 16384;  // the sim's level; the room sets the level heard

kq::Bytes randomVoucher() {
    kq::Bytes v(32);
    for (auto& b : v) b = (uint8_t)room_rand();
    return v;
}

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

struct Point {
    int fail = 0, wrong = 0, ok = 0, tries = 0;
    long fixed = 0;
    double macs = 0, ms = 0, late_ms = 0, late_max = 0;
};

// One frame through the room at snr, lead noise before it; decoded blind, or streamed in 20 ms
// blocks through fskv_rx. late_ms: how long after the frame's end the receiver had it.
Point trial(const fskv_chan& c, double snr, int t, bool stream, Point p) {
    kq::Bytes v = randomVoucher();
    fskv_modulate(v.data(), kAmp, g_tx);
    size_t lead = stream ? (size_t)(16000 * (0.5 + 2.5 * room_uniform())) : (size_t)(16000 * (0.2 + 0.4 * room_uniform()));
    double t0;
    size_t n = room_channel(g_tx, FSKV_SAMPLES, lead, 6400, &c, t & 1 ? 0.005 : -0.005, snr, g_rx, kMax, &t0);
    uint8_t out[32];
    fskv_stats st{};
    auto clk = std::chrono::steady_clock::now();
    int got = 0;
    if (!stream) {
        got = fskv_decode(g_rx, n, out, &st);
    } else {
        static int16_t buf[FSKV_RX_MIN];
        static fskv_rx rx;
        fskv_rx_init(&rx, buf, sizeof buf / sizeof *buf);
        size_t at = 0;
        for (; at < n && !got; at += 320) got = fskv_rx_push(&rx, g_rx + at, std::min<size_t>(320, n - at), out);
        st = rx.st;
        if (got) {
            double late = (at - (t0 + FSKV_SAMPLES * (1 + (t & 1 ? 0.005 : -0.005)))) / 16.0;
            p.late_ms += late;
            p.late_max = std::max(p.late_max, late);
        }
    }
    p.ms += msSince(clk);
    p.macs += st.macs;
    if (!got) p.fail++;
    else if (memcmp(out, v.data(), 32)) p.wrong++;
    else { p.ok++; p.fixed += st.rs_fixed; p.tries += st.tries; }
    return p;
}

Point run(const fskv_chan& c, double snr, int n, bool stream) {
    Point p;
    for (int t = 0; t < n; t++) p = trial(c, snr, t, stream, p);
    return p;
}

void sweep(int n, const fskv_chan& c) {
    static const double room[] = {-6, -3, 0, 3, 6, 10, 15, 20}, few[] = {0, 6, 12, 20, 30};
    const double* snrs = &c == &FSKV_STRESS ? few : room;
    int ns = &c == &FSKV_STRESS ? 5 : 8;
    printf("channel %s: rt60 %.2f s, DRR %.0f dB, early reflections up to %.2f, drift +-0.5%%, %d trials a point\n",
           c.name, c.rt60, c.drr_db, c.early, n);
    printf("%-7s %6s %7s %6s %9s %9s %9s %8s %9s\n", "rx", "snr_dB", "FER", "wrong", "rsfix_avg", "tries_avg",
           "Mmac_avg", "ms_avg", "late_ms");
    for (int stream = 0; stream < 2; stream++)
        for (int s = 0; s < ns; s++) {
            Point p = run(c, snrs[s], n, stream);
            printf("%-7s %6.1f %7.3f %6d %9.2f %9.2f %9.1f %8.1f %9s\n", stream ? "stream" : "blind", snrs[s],
                   (double)(p.fail + p.wrong) / n, p.wrong, p.ok ? (double)p.fixed / p.ok : 0,
                   p.ok ? (double)p.tries / p.ok : 0, p.macs / n / 1e6, p.ms / n,
                   stream && p.ok ? (std::to_string((int)(p.late_ms / p.ok)) + "/" + std::to_string((int)p.late_max)).c_str() : "-");
            fflush(stdout);
        }
}

const char* kFixtures[] = {"tx", "room-10dB", "room-0dB"};

void fixtures(const std::string& dir, const kq::Bytes& v) {
    fskv_modulate(v.data(), kAmp, g_tx);
    memset(g_rx, 0, sizeof g_rx);  // as a device plays it: 0.2 s of quiet first, 0.5 s after
    memcpy(g_rx + 3200, g_tx, sizeof g_tx);
    wav_write((dir + "/tx.wav").c_str(), g_rx, 3200 + FSKV_SAMPLES + 8000);
    room_seed(7);
    double t0;
    size_t n = room_channel(g_tx, FSKV_SAMPLES, 4000, 6400, &FSKV_ROOM, 0.005, 10, g_rx, kMax, &t0);
    wav_write((dir + "/room-10dB.wav").c_str(), g_rx, n);
    n = room_channel(g_tx, FSKV_SAMPLES, 7000, 6400, &FSKV_ROOM, -0.005, 0, g_rx, kMax, &t0);
    wav_write((dir + "/room-0dB.wav").c_str(), g_rx, n);
}

int decodeFile(const char* path) {
    size_t n = wav_read(path, g_rx, kMax);
    if (!n) { fprintf(stderr, "%s: not a 16 kHz mono PCM16 WAV\n", path); return 2; }
    uint8_t out[32];
    fskv_stats st{};
    if (!fskv_decode(g_rx, n, out, &st)) { printf("%s: no voucher (%zu samples)\n", path, n); return 1; }
    std::string code = kq1::voucher_code(kq::Bytes(out, out + 32));
    printf("%s: %s, check %s: at %.0f, clock %.4f, sync %.2f, RS fixed %d, %d tries\n", path, code.c_str(),
           kq1::voucher_check(code).c_str(), st.t0, st.a, st.metric, st.rs_fixed, st.tries);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Vec vec{"fskv", "modem", {}};
    kq::Bytes voucher;
    for (auto& v : load(argv[1], "voucher/voucher_vectors_v2.json"))
        if (v.name == "mint") voucher = v.hex("voucher");
    std::string mode = argc > 2 ? argv[2] : "";
    if (mode == "sweep" && argc > 3) {
        std::string ch = argc > 4 ? argv[4] : "room";
        sweep(atoi(argv[3]), ch == "stress" ? FSKV_STRESS : ch == "dry" ? FSKV_DRY : FSKV_ROOM);
        return 0;
    }
    if (mode == "fixtures" && argc > 3) { fixtures(argv[3], voucher); return 0; }
    if (mode == "decode" && argc > 3) return decodeFile(argv[3]);
    if (mode == "room" && argc > 5) {
        static int16_t in[kMax];
        size_t n = wav_read(argv[3], in, kMax / 2);
        double t0;
        size_t m = n ? room_channel(in, n, 1600, 6400, &FSKV_ROOM, argc > 6 ? atof(argv[6]) : 0.005, atof(argv[5]), g_rx, kMax, &t0) : 0;
        return m && wav_write(argv[4], g_rx, m) ? 0 : 2;
    }

    // RS: up to 8 byte errors are fixed; with 9 to 16, RS, the length and the CRC let none through.
    int bad = 0, wrong = 0;
    for (int it = 0; it < 20000; it++) {
        uint8_t msg[FSKV_FRAME], cw[FSKV_CW], rx[FSKV_CW];
        for (auto& b : msg) b = (uint8_t)room_rand();
        fskv_rs_encode(msg, FSKV_FRAME, cw);
        memcpy(rx, cw, FSKV_CW);
        int ne = it % 9;
        for (int e = 0; e < ne; e++) {
            int pos;
            do pos = (int)(room_rand() % FSKV_CW); while (rx[pos] != cw[pos]);
            rx[pos] ^= (uint8_t)(1 + room_rand() % 255);
        }
        bad += fskv_rs_decode(rx, FSKV_CW) != ne || memcmp(rx, cw, FSKV_CW);
    }
    check(bad == 0, vec, "RS fixes 0 to 8 byte errors in 20000 words");
    for (int it = 0; it < 20000; it++) {
        uint8_t v[32], cw[FSKV_CW];
        for (auto& b : v) b = (uint8_t)room_rand();
        fskv_frame(v, cw);
        int ne = 9 + it % 8;
        for (int e = 0; e < ne; e++) cw[room_rand() % FSKV_CW] ^= (uint8_t)(1 + room_rand() % 255);
        if (fskv_rs_decode(cw, FSKV_CW) < 0 || cw[0] != 32) continue;
        uint16_t c = fskv_crc16(cw, 33);
        wrong += cw[33] == (c >> 8) && cw[34] == (c & 0xFF) && memcmp(cw + 1, v, 32);
    }
    check(wrong == 0, vec, "no frame with 9 to 16 byte errors passes as another voucher, in 20000");
    check(fskv_crc16((const uint8_t*)"123456789", 9) == 0x29B1, vec, "CRC-16/CCITT-FALSE check value");

    // The FFT against the DFT's definition at every tone's bin.
    {
        int16_t x[512];
        for (auto& s : x) s = (int16_t)(room_rand() % 20000) - 10000;
        float p[256];
        fskv_spectrum(x, p);
        double worst = 0;
        for (int b = 32; b < 160; b += 2) {
            double re = 0, im = 0;
            for (int j = 0; j < 512; j++) { re += x[j] * cos(2 * M_PI * b * j / 512); im -= x[j] * sin(2 * M_PI * b * j / 512); }
            worst = std::max(worst, fabs(p[b] - (re * re + im * im)) / (re * re + im * im));
        }
        check(worst < 1e-3, vec, "the FFT's power at the tones' bins is the DFT's");
    }

    // A clean frame between silences; its level.
    {
        fskv_modulate(voucher.data(), 9268, g_tx);
        double e = 0;
        int peak = 0;
        for (int i = 0; i < FSKV_SAMPLES; i++) { e += (double)g_tx[i] * g_tx[i]; peak = std::max(peak, abs(g_tx[i])); }
        double dbfs = 20 * log10(sqrt(e / FSKV_SAMPLES) / 32768);
        check(fabs(dbfs + 20) < 0.5 && peak <= 9268, vec, "amplitude 9268 is -20 dBFS RMS");
        printf("clip: %d samples (%.2f s), %.1f dBFS RMS, peak %d\n", FSKV_SAMPLES, FSKV_SAMPLES / 16000.0, dbfs, peak);
        memset(g_rx, 0, sizeof g_rx);
        memcpy(g_rx + 3000, g_tx, sizeof g_tx);
        uint8_t out[32];
        fskv_stats st{};
        check(fskv_decode(g_rx, FSKV_SAMPLES + 6000, out, &st) && !memcmp(out, voucher.data(), 32), vec,
              "clean loopback");
        printf("clean loopback: at %.1f (sent at 3000), clock %.4f, RS fixed %d, %d tries\n", st.t0, st.a, st.rs_fixed,
               st.tries);
        // Any start up to the 8 ms guard early puts every window inside its symbol: as good, with no echo.
        check(st.t0 >= 3000 - 128 && st.t0 <= 3002 && fabs(st.a - 1) <= 0.001 && st.rs_fixed == 0 && st.tries == 1,
              vec, "found within the guard of where it starts, at the nominal clock, first try");
    }

    // The fixtures, made by `fixtures`: the vector's voucher, clean and through the room.
    for (const char* f : kFixtures) {
        std::string path = std::string("vectors/fskv/") + f + ".wav";
        size_t n = wav_read(path.c_str(), g_rx, kMax);
        uint8_t out[32];
        fskv_stats st{};
        check(n && fskv_decode(g_rx, n, out, &st) && !memcmp(out, voucher.data(), 32), vec, (path + " decodes").c_str());
        printf("%s: clock %.4f, RS fixed %d, %d tries\n", path.c_str(), st.a, st.rs_fixed, st.tries);
    }

    // The room, drift +-0.5%: blind and streamed.
    room_seed(1);
    for (int stream = 0; stream < 2; stream++)
        for (double snr : {0.0, 3.0, 10.0, 20.0}) {
            Point p = run(FSKV_ROOM, snr, 12, stream);
            char what[96];
            snprintf(what, sizeof what, "%s, room at %.0f dB SNR: %d of 12 lost", stream ? "stream" : "blind", snr,
                     p.fail + p.wrong);
            check(p.wrong == 0 && p.fail <= (snr < 3 ? 1 : 0), vec, what);
            printf("%s: RS fixed %.2f, %.2f tries, %.1f Mmac, %.0f ms a frame", what, p.ok ? (double)p.fixed / p.ok : 0,
                   p.ok ? (double)p.tries / p.ok : 0, p.macs / 12 / 1e6, p.ms / 12);
            if (stream) printf(", heard %.0f ms after its end at most", p.late_max);
            printf("\n");
            if (stream) check(p.late_max < 600, vec, "streamed: heard within 0.6 s of the frame's end");
        }

    // Noise alone, as long as a listen lasts, four times: nothing heard.
    {
        static int16_t buf[FSKV_RX_MIN];
        static fskv_rx rx;
        int heard = 0, peaks = 0;
        for (int i = 0; i < 4; i++) {
            room_noise(0, g_rx, 16000 * 15);
            fskv_rx_init(&rx, buf, sizeof buf / sizeof *buf);
            uint8_t out[32];
            for (size_t at = 0; at < 16000 * 15; at += 320) {
                heard += fskv_rx_push(&rx, g_rx + at, 320, out);
                peaks += rx.peak > 0;
            }
            heard += fskv_decode(g_rx, 16000 * 15, out, nullptr);
        }
        check(heard == 0, vec, "no voucher in 60 s of noise");
        printf("noise: 60 s, %d blocks with a peak waiting\n", peaks);
    }

    // Two devices show the same digits for one voucher; the vector's, from Python's hashlib.
    std::string code = kq1::voucher_code(voucher);
    check(kq1::voucher_check(code) == "1939", vec, "check code of the vector's voucher");
    check(kq1::voucher_check("AAAA").empty() && kq1::voucher_check("not a code").empty(), vec,
          "no check code for what is not a voucher code");
    return finish("fskv");
}
