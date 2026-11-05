// SPDX-License-Identifier: AGPL-3.0-only
#include "fskv_room.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const fskv_chan FSKV_ROOM = {"room", 0.3, 10, 1, 0.3}, FSKV_STRESS = {"stress", 0.6, 0, 1, 0.55},
                FSKV_DRY = {"dry", 0, 0, 1, 0};

static uint64_t rng = 0x9E3779B97F4A7C15ull;
void room_seed(uint64_t s) { rng = s ? s : 0x9E3779B97F4A7C15ull; }
uint64_t room_rand(void) {
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1Dull;
}
double room_uniform(void) { return (room_rand() >> 11) * (1.0 / 9007199254740992.0); }
double room_gauss(void) {
    double u = room_uniform() + 1e-300, v = room_uniform();
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

// ---- biquads (RBJ cookbook) ----
typedef struct { double b0, b1, b2, a1, a2, z1, z2; } bq;
static bq bq_make(int type, double f, double q, double gain_db, int fs) {
    double w = 2 * M_PI * f / fs, al = sin(w) / (2 * q), c = cos(w), A = pow(10, gain_db / 40), b0, b1, b2, a0, a1, a2;
    if (type == 0) { b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; }          // LP
    else if (type == 1) { b0 = (1 + c) / 2; b1 = -(1 + c); b2 = b0; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; }  // HP
    else { b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A; }    // peak
    bq r = {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, 0, 0};
    return r;
}
static void bq_run(bq* f, double* x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        double y = f->b0 * x[i] + f->z1;
        f->z1 = f->b1 * x[i] - f->a1 * y + f->z2;
        f->z2 = f->b2 * x[i] - f->a2 * y;
        x[i] = y;
    }
}

// x (nx samples) -> y, time-stretched by a (y[n] = x(n/a)); a 32-tap windowed sinc.
static size_t resample(const double* x, size_t nx, double a, double* y) {
    size_t ny = (size_t)(nx * a);
    for (size_t n = 0; n < ny; n++) {
        double u = n / a, acc = 0;
        long i0 = (long)floor(u);
        for (long k = i0 - 15; k <= i0 + 16; k++) {
            if (k < 0 || k >= (long)nx || x[k] == 0) continue;
            double d = u - k, s = fabs(d) < 1e-12 ? 1 : sin(M_PI * d) / (M_PI * d);
            acc += x[k] * s * (0.5 + 0.5 * cos(M_PI * d / 16.5));
        }
        y[n] = acc;
    }
    return ny;
}

// Impulse response: direct path, six early reflections in 1-15 ms, exponential noise tail.
static size_t make_ir(const fskv_chan* c, int fs, double* h, size_t cap) {
    size_t L = (size_t)(c->rt60 * fs * 2 / 3);  // to -40 dB
    if (L > cap) L = cap;
    memset(h, 0, L * sizeof *h);
    h[0] = 1;
    for (int i = 0; i < 6; i++)
        h[(size_t)(fs * (0.001 + 0.014 * room_uniform()))] += (room_uniform() < 0.5 ? -1 : 1) * c->early * (0.4 + 0.6 * room_uniform());
    double e = 0, start = 0.005 * fs;
    for (size_t i = (size_t)start; i < L; i++) {
        h[i] += room_gauss() * exp(-6.9 * (i - start) / (c->rt60 * fs));
        e += h[i] * h[i];
    }
    double want = pow(10, -c->drr_db / 10), g = sqrt(want / (e + 1e-12));
    for (size_t i = (size_t)start; i < L; i++) h[i] *= g;
    return L;
}

#define FS 16000
#define MAXN (FS * 12)
static double bufA[MAXN], bufB[MAXN], ir[FS];
static const double kSig = 32768 * 0.0316227766;  // -30 dBFS RMS

size_t room_channel(const int16_t* tx, size_t ntx, size_t lead, size_t tail, const fskv_chan* c, double drift,
                    double snr_db, int16_t* rx, size_t cap, double* t0) {
    size_t nx = lead + ntx + tail;
    double a = 1 + drift;
    if ((size_t)(nx * a) + 1 > MAXN || (size_t)(nx * a) > cap) return 0;
    memset(bufA, 0, nx * sizeof *bufA);
    for (size_t i = 0; i < ntx; i++) bufA[lead + i] = tx[i];
    size_t n = resample(bufA, nx, a, bufB);
    if (c->band) {  // small speaker and MEMS mic: 300 Hz - 6 kHz, a peak and a dip
        bq f[6] = {bq_make(1, 300, 0.707, 0, FS), bq_make(1, 300, 0.707, 0, FS), bq_make(0, 6000, 0.707, 0, FS),
                   bq_make(0, 6000, 0.707, 0, FS), bq_make(2, 2500, 1.5, 6, FS), bq_make(2, 4500, 2, -6, FS)};
        for (int i = 0; i < 6; i++) bq_run(&f[i], bufB, n);
    }
    if (c->rt60 > 0) {
        size_t L = make_ir(c, FS, ir, sizeof ir / sizeof *ir);
        memset(bufA, 0, n * sizeof *bufA);
        for (size_t i = 0; i < n; i++) {
            if (bufB[i] == 0) continue;
            size_t e = i + L < n ? L : n - i;
            for (size_t k = 0; k < e; k++) bufA[i + k] += bufB[i] * ir[k];
        }
        memcpy(bufB, bufA, n * sizeof *bufB);
    }
    size_t s0 = (size_t)(lead * a), s1 = (size_t)((lead + ntx) * a);
    double p = 0;
    for (size_t i = s0; i < s1; i++) p += bufB[i] * bufB[i];
    p /= (double)(s1 - s0);
    double g = kSig / sqrt(p), sn = kSig / pow(10, snr_db / 20);
    for (size_t i = 0; i < n; i++) {
        double y = bufB[i] * g + sn * room_gauss();
        rx[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : lround(y));
    }
    *t0 = lead * a;
    return n;
}

void room_noise(double snr_db, int16_t* x, size_t n) {
    double sn = kSig / pow(10, snr_db / 20);
    for (size_t i = 0; i < n; i++) {
        double y = sn * room_gauss();
        x[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : lround(y));
    }
}

int wav_write(const char* path, const int16_t* x, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t d = (uint32_t)n * 2, r = 36 + d, b = FS * 2, fmt = 16, rate = FS;
    uint16_t pcm = 1, ch = 1, al = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&r, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&b, 4, 1, f);
    fwrite(&al, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f);
    fwrite(x, 2, n, f);
    return fclose(f) == 0;
}

// The 44-byte header wav_write writes, as sim.c and most recorders write it.
size_t wav_read(const char* path, int16_t* x, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[44];
    size_t n = 0;
    if (fread(h, 1, 44, f) == 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && h[20] == 1 &&
        h[22] == 1 && (h[24] | h[25] << 8) == FS && h[34] == 16 && !memcmp(h + 36, "data", 4))
        n = fread(x, 2, cap, f);
    fclose(f);
    return n;
}
