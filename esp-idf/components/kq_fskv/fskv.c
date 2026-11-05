// SPDX-License-Identifier: AGPL-3.0-only
#include "fskv.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f
#define GROUPS 4
#define TONES 16
#define BPS 16                    // bits a symbol: 4 a group
#define KS 4                      // sync symbols
#define KD (FSKV_SYMBOLS - KS)    // data symbols
#define F0 1000.0f
#define DF 62.5f
#define GDF 1000.0f
#define BIN(g, t) (32 + 32 * (g) + 2 * (t))  // tone t of group g in the 512-point DFT at 16 kHz

// The sync word: its 16 nibbles are 0..15 once each, so every tone is trained.
static const uint8_t kSync[8] = {0x3A, 0xF1, 0x6C, 0x08, 0xD5, 0x2E, 0x94, 0xB7};

// ---- CRC-16/CCITT-FALSE ----
uint16_t fskv_crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

// ---- Reed-Solomon over GF(256), x^8+x^4+x^3+x^2+1, roots a^0..a^(NSYM-1) ----
static uint8_t gexp[512], glog[256];
static uint8_t ggen[FSKV_NSYM + 1];  // generator, highest degree first
static int ginit;

static uint8_t gmul(uint8_t a, uint8_t b) { return (a && b) ? gexp[glog[a] + glog[b]] : 0; }
static uint8_t gdiv(uint8_t a, uint8_t b) { return a ? gexp[(glog[a] + 255 - glog[b]) % 255] : 0; }

static void gf_init(void) {
    if (ginit) return;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gexp[i] = (uint8_t)x;
        glog[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    for (int i = 255; i < 512; i++) gexp[i] = gexp[i - 255];
    memset(ggen, 0, sizeof ggen);
    ggen[0] = 1;
    for (int i = 0; i < FSKV_NSYM; i++)  // ggen *= (x + a^i)
        for (int j = i + 1; j > 0; j--) ggen[j] ^= gmul(ggen[j - 1], gexp[i]);
    ginit = 1;
}

void fskv_rs_encode(const uint8_t* msg, size_t k, uint8_t* cw) {
    gf_init();
    uint8_t rem[FSKV_NSYM] = {0};
    for (size_t i = 0; i < k; i++) {
        uint8_t fb = msg[i] ^ rem[0];
        memmove(rem, rem + 1, FSKV_NSYM - 1);
        rem[FSKV_NSYM - 1] = 0;
        for (int j = 0; j < FSKV_NSYM; j++) rem[j] ^= gmul(ggen[j + 1], fb);
    }
    memmove(cw, msg, k);
    memcpy(cw + k, rem, FSKV_NSYM);
}

static int syndromes(const uint8_t* cw, size_t n, uint8_t* s) {
    int any = 0;
    for (int i = 0; i < FSKV_NSYM; i++) {
        uint8_t v = 0;
        for (size_t j = 0; j < n; j++) v = gmul(v, gexp[i]) ^ cw[j];
        s[i] = v;
        any |= v;
    }
    return any;
}

int fskv_rs_decode(uint8_t* cw, size_t n) {
    gf_init();
    uint8_t S[FSKV_NSYM];
    if (!syndromes(cw, n, S)) return 0;
    // Berlekamp-Massey; polynomials lowest degree first. Byte cw[i] is the coefficient of x^(n-1-i).
    uint8_t C[FSKV_NSYM + 1] = {1}, B[FSKV_NSYM + 1] = {1}, T[FSKV_NSYM + 1];
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int r = 0; r < FSKV_NSYM; r++) {
        uint8_t d = S[r];
        for (int i = 1; i <= L; i++) d ^= gmul(C[i], S[r - i]);
        if (!d) { m++; continue; }
        uint8_t coef = gdiv(d, b);
        memcpy(T, C, sizeof C);
        for (int i = 0; i + m <= FSKV_NSYM; i++) C[i + m] ^= gmul(coef, B[i]);
        if (2 * L <= r) {
            L = r + 1 - L;
            memcpy(B, T, sizeof B);
            b = d;
            m = 1;
        } else {
            m++;
        }
    }
    if (2 * L > FSKV_NSYM) return -1;
    // Omega = S * Lambda mod x^NSYM
    uint8_t O[FSKV_NSYM] = {0};
    for (int i = 0; i < FSKV_NSYM; i++)
        for (int j = 0; j <= i && j <= L; j++) O[i] ^= gmul(C[j], S[i - j]);
    int found = 0;
    for (size_t p = 0; p < n; p++) {  // Chien search over position p (power of x)
        uint8_t xi = gexp[(255 - (p % 255)) % 255];  // X^-1
        uint8_t lv = 0, xp = 1;
        for (int i = 0; i <= L; i++) { lv ^= gmul(C[i], xp); xp = gmul(xp, xi); }
        if (lv) continue;
        uint8_t ov = 0, dv = 0;
        xp = 1;
        for (int i = 0; i < FSKV_NSYM; i++) { ov ^= gmul(O[i], xp); xp = gmul(xp, xi); }
        xp = 1;  // Lambda'(x) = sum over odd i of C[i] x^(i-1)
        for (int i = 1; i <= L; i += 2) { dv ^= gmul(C[i], xp); xp = gmul(xp, gmul(xi, xi)); }
        if (!dv) return -1;
        cw[n - 1 - p] ^= gmul(gexp[p % 255], gdiv(ov, dv));  // Forney, first root a^0
        found++;
    }
    if (found != L || syndromes(cw, n, S)) return -1;
    return found;
}

// ---- framing ----
static int getbit(const uint8_t* p, size_t nbits, size_t i) { return i < nbits ? (p[i >> 3] >> (7 - (i & 7))) & 1 : 0; }

// Tone of group g in symbol k of the frame (sync, then the coded bytes; cw NULL: the sync only).
static int tone_of(const uint8_t* cw, int k, int g) {
    const uint8_t* src = k < KS ? kSync : cw;
    size_t nb = k < KS ? 64 : FSKV_CW * 8, base = (size_t)(k < KS ? k : k - KS) * BPS + (size_t)g * 4;
    int t = 0;
    for (int i = 0; i < 4; i++) t = (t << 1) | getbit(src, nb, base + i);
    return t;
}

void fskv_frame(const uint8_t v[FSKV_PAYLOAD], uint8_t cw[FSKV_CW]) {
    uint8_t f[FSKV_FRAME];
    f[0] = FSKV_PAYLOAD;
    memcpy(f + 1, v, FSKV_PAYLOAD);
    uint16_t c = fskv_crc16(f, 1 + FSKV_PAYLOAD);
    f[FSKV_FRAME - 2] = (uint8_t)(c >> 8);
    f[FSKV_FRAME - 1] = (uint8_t)c;
    fskv_rs_encode(f, FSKV_FRAME, cw);
}

// Continuous phase, with 2 ms ramps at either end.
void fskv_modulate(const uint8_t v[FSKV_PAYLOAD], float amp, int16_t* out) {
    uint8_t cw[FSKV_CW];
    fskv_frame(v, cw);
    float ph[GROUPS] = {0};
    const int ramp = FSKV_FS / 500, N = FSKV_SAMPLES;
    for (int k = 0; k < FSKV_SYMBOLS; k++) {
        float w[GROUPS];
        for (int g = 0; g < GROUPS; g++) w[g] = 2 * PI_F * (F0 + g * GDF + tone_of(cw, k, g) * DF) / FSKV_FS;
        for (int n = k * FSKV_SPS; n < (k + 1) * FSKV_SPS; n++) {
            float y = 0;
            for (int g = 0; g < GROUPS; g++) {
                ph[g] += w[g];
                if (ph[g] > 2 * PI_F) ph[g] -= 2 * PI_F;
                y += sinf(ph[g]);
            }
            float r = 1;
            if (n < ramp) r = 0.5f - 0.5f * cosf(PI_F * n / ramp);
            if (N - n < ramp) r = 0.5f - 0.5f * cosf(PI_F * (N - n) / ramp);
            out[n] = (int16_t)lrintf(amp / GROUPS * y * r);
        }
    }
}

// ---- demodulation ----
// Tone energies E[g*16+t] of symbol k of a frame at t0, clock ratio a (RX samples per TX sample).
static int energies(const int16_t* x, size_t n, double t0, double a, int k, float* E, long* macs) {
    long i0 = lround(t0 + (k * FSKV_SPS + FSKV_SPS - FSKV_WIN) * a);
    int L = (int)lround(FSKV_WIN * a);
    if (i0 < 0 || (size_t)(i0 + L) > n) return 0;
    for (int g = 0; g < GROUPS; g++)
        for (int t = 0; t < TONES; t++) {
            float w = 2 * PI_F * (F0 + g * GDF + t * DF) / (float)(a * FSKV_FS);
            float c = cosf(w), s = sinf(w), zr = 1, zi = 0, ar = 0, ai = 0;
            const int16_t* p = x + i0;
            for (int j = 0; j < L; j++) {
                ar += p[j] * zr;
                ai -= p[j] * zi;
                float nr = zr * c - zi * s;
                zi = zr * s + zi * c;
                zr = nr;
            }
            E[g * TONES + t] = ar * ar + ai * ai;
        }
    if (macs) *macs += (long)FSKV_TONES * L;
    return 1;
}

// How much more the expected tone holds than the others: about 1 on the sync word, 0 on noise.
static double contrast(const float* E, int k) {
    double sum = 0;
    for (int g = 0; g < GROUPS; g++) {
        const float* e = E + g * TONES;
        double tot = 1e-9;
        for (int t = 0; t < TONES; t++) tot += e[t];
        double ex = e[tone_of(NULL, k, g)];
        sum += (ex - (tot - ex) / (TONES - 1)) / tot;
    }
    return sum / GROUPS;
}

static double sync_metric(const int16_t* x, size_t n, double t0, double a, long* macs) {
    float E[FSKV_TONES];
    double sum = 0;
    for (int k = 0; k < KS; k++) {
        if (!energies(x, n, t0, a, k, E, macs)) return -1;
        sum += contrast(E, k);
    }
    return sum / KS;
}

static double refine(const int16_t* x, size_t n, double lo, double hi, double step, double a, double* t0,
                     long* macs) {
    double best = -2;
    for (double t = lo; t <= hi; t += step) {
        double v = sync_metric(x, n, t, a, macs);
        if (v > best) { best = v; *t0 = t; }
    }
    return best;
}
double fskv_refine(const int16_t* x, size_t n, double lo, double hi, double step, double a, double* t0) {
    return refine(x, n, lo, hi, step, a, t0, NULL);
}

// Hard decisions on data symbols [k0, k1) into cw (when given); returns the mean share of each
// group's energy in its strongest tone.
static double demod(const int16_t* x, size_t n, double t0, double a, int k0, int k1, uint8_t* cw, long* macs) {
    float E[FSKV_TONES];
    double share = 0;
    if (cw) memset(cw, 0, FSKV_CW);
    for (int k = k0; k < k1; k++) {
        if (!energies(x, n, t0, a, KS + k, E, macs)) memset(E, 0, sizeof E);
        for (int g = 0; g < GROUPS; g++) {
            int bt = 0;
            for (int t = 1; t < TONES; t++)
                if (E[g * TONES + t] > E[g * TONES + bt]) bt = t;
            double tot = 1e-9;
            for (int t = 0; t < TONES; t++) tot += E[g * TONES + t];
            share += E[g * TONES + bt] / tot;
            if (!cw) continue;
            for (int i = 0; i < 4; i++) {
                size_t bit = (size_t)k * BPS + (size_t)g * 4 + i;
                if (bit < FSKV_CW * 8 && ((bt >> (3 - i)) & 1)) cw[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
            }
        }
    }
    return share / ((k1 - k0) * GROUPS);
}

void fskv_demod_bits(const int16_t* x, size_t n, double t0, double a, uint8_t* bits) {
    uint8_t cw[FSKV_CW];
    demod(x, n, t0, a, 0, KD, cw, NULL);
    for (int i = 0; i < FSKV_CW * 8; i++) bits[i] = (uint8_t)getbit(cw, FSKV_CW * 8, (size_t)i);
}

#define DRIFT_MAX 0.007  // the clock ratios tried: 1 +- 0.7% in steps of 0.05%
#define DRIFT_STEP 0.0005
#define NA (2 * (int)(DRIFT_MAX / DRIFT_STEP + 0.5) + 1)
#define TAIL 8           // data symbols at the end of the frame that rank the clock ratios
#define STEP (FSKV_WIN / 8)

// Drift shows most at the end of the frame (0.5% is 96 samples there, most of the guard), so the
// ratios go to RS cleanest tail first: the right one is nearly always the first tried.
int fskv_decode_at(const int16_t* x, size_t n, double t, uint8_t v[FSKV_PAYLOAD], fskv_stats* st) {
    fskv_stats s = {t, 1, -2, -1, 0, st ? st->macs : 0};
    double t1 = t;
    s.metric = refine(x, n, t - 2 * STEP, t + 2 * STEP, STEP / 4, 1, &t1, &s.macs);
    double sh[NA];
    for (int i = 0; i < NA; i++) sh[i] = demod(x, n, t1, 1 + (i - NA / 2) * DRIFT_STEP, KD - TAIL, KD, NULL, &s.macs);
    for (s.tries = 1; s.tries <= NA; s.tries++) {
        int i = 0;
        for (int j = 1; j < NA; j++) if (sh[j] > sh[i]) i = j;
        sh[i] = -1;
        double a = 1 + (i - NA / 2) * DRIFT_STEP, ts = t1;
        refine(x, n, t1 - 3, t1 + 3, 1, a, &ts, &s.macs);
        uint8_t cw[FSKV_CW];
        demod(x, n, ts, a, 0, KD, cw, &s.macs);
        int fixed = fskv_rs_decode(cw, FSKV_CW);
        if (fixed < 0 || cw[0] != FSKV_PAYLOAD) continue;
        uint16_t crc = fskv_crc16(cw, 1 + FSKV_PAYLOAD);
        if (cw[FSKV_FRAME - 2] != (crc >> 8) || cw[FSKV_FRAME - 1] != (crc & 0xFF)) continue;
        memcpy(v, cw + 1, FSKV_PAYLOAD);
        s.t0 = ts;
        s.a = a;
        s.rs_fixed = fixed;
        if (st) *st = s;
        return 1;
    }
    s.tries = NA;
    if (st) *st = s;
    return 0;
}

#define NCAND 3

int fskv_decode(const int16_t* x, size_t n, uint8_t v[FSKV_PAYLOAD], fskv_stats* st) {
    fskv_stats s = {0, 1, -2, -1, 0, 0};
    double ct[NCAND], cm[NCAND];
    for (int i = 0; i < NCAND; i++) { ct[i] = -1; cm[i] = -2; }
    // Coarse scan at the nominal clock: the sync word is short enough for drift not to matter.
    // One symbol screens a position before all four are measured.
    for (double t = 0; t + FSKV_SPAN < n; t += STEP) {
        float E[FSKV_TONES];
        if (!energies(x, n, t, 1, 0, E, &s.macs) || contrast(E, 0) < 0.1) continue;
        double v1 = sync_metric(x, n, t, 1, &s.macs);
        int slot = -1;
        for (int i = 0; i < NCAND; i++)
            if (ct[i] >= 0 && fabs(ct[i] - t) < KS * FSKV_SPS) { slot = cm[i] < v1 ? i : -2; break; }
        if (slot == -1) {  // a new peak: replace the weakest
            slot = 0;
            for (int i = 1; i < NCAND; i++) if (cm[i] < cm[slot]) slot = i;
            if (cm[slot] >= v1) slot = -2;
        }
        if (slot >= 0) { ct[slot] = t; cm[slot] = v1; }
    }
    for (int r = 0; r < NCAND; r++) {
        int c = 0;
        for (int i = 1; i < NCAND; i++) if (cm[i] > cm[c]) c = i;
        if (ct[c] < 0 || cm[c] < 0.2) break;
        s.t0 = ct[c];
        if (fskv_decode_at(x, n, ct[c], v, &s)) {
            if (st) *st = s;
            return 1;
        }
        cm[c] = -2;
        ct[c] = -1;
    }
    if (st) *st = s;
    return 0;
}

// ---- the 512-point FFT, radix 2 ----
static float twc[256], tws[256];
static int twinit;

void fskv_spectrum(const int16_t* x, float* p) {
    float re[512], im[512];
    if (!twinit) {
        for (int i = 0; i < 256; i++) { twc[i] = cosf(2 * PI_F * i / 512); tws[i] = sinf(2 * PI_F * i / 512); }
        twinit = 1;
    }
    for (int i = 0; i < 512; i++) {
        int j = 0;
        for (int b = 0; b < 9; b++) j |= ((i >> b) & 1) << (8 - b);
        re[j] = x[i];
        im[j] = 0;
    }
    for (int len = 2; len <= 512; len <<= 1) {
        int half = len / 2, step = 512 / len;
        for (int i = 0; i < 512; i += len)
            for (int k = 0; k < half; k++) {
                float wr = twc[k * step], wi = -tws[k * step];
                int a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
    }
    for (int k = 0; k < 256; k++) p[k] = re[k] * re[k] + im[k] * im[k];
}

// ---- the receiver ----
// A frame at hop h0 has its sync symbols' windows at hops h0+2, +12, +22, +32 (640 = 10 hops).
#define THRESH 0.2

void fskv_rx_init(fskv_rx* r, int16_t* buf, size_t cap) {
    memset(r, 0, sizeof *r);
    r->buf = buf;
    r->cap = cap;
}

static double rx_metric(const fskv_rx* r, uint64_t h0) {
    double sum = 0;
    for (int k = 0; k < KS; k++) sum += contrast(r->e[(h0 + 2 + 10 * k) % FSKV_HIST], k);
    return sum / KS;
}

int fskv_rx_push(fskv_rx* r, const int16_t* x, size_t n, uint8_t v[FSKV_PAYLOAD]) {
    while (n) {
        if (r->len == r->cap) {  // keep what a waiting frame or the next sync word may still need
            uint64_t keep = r->hop > 40 ? (r->hop - 40) * FSKV_HOP : 0;
            if (r->peak > 0 && r->peak_at < keep + 256) keep = r->peak_at > 256 ? r->peak_at - 256 : 0;
            if (keep <= r->base) keep = r->base + r->cap / 4;  // no room: the waiting frame is dropped
            size_t drop = (size_t)(keep - r->base);
            if (drop > r->len) drop = r->len;
            memmove(r->buf, r->buf + drop, (r->len - drop) * sizeof *r->buf);
            r->len -= drop;
            r->base += drop;
            if (r->peak > 0 && r->peak_at < r->base + 256) r->peak = 0;
            if (r->hop * FSKV_HOP < r->base) r->hop = (r->base + FSKV_HOP - 1) / FSKV_HOP;
        }
        size_t m = r->cap - r->len < n ? r->cap - r->len : n;
        memcpy(r->buf + r->len, x, m * sizeof *x);
        r->len += m;
        x += m;
        n -= m;
        uint64_t end = r->base + r->len;
        while (r->hop * FSKV_HOP + FSKV_WIN <= end) {
            float p[256];
            fskv_spectrum(r->buf + (r->hop * FSKV_HOP - r->base), p);
            r->ffts++;
            float* e = r->e[r->hop % FSKV_HIST];
            for (int g = 0; g < GROUPS; g++)
                for (int t = 0; t < TONES; t++) e[g * TONES + t] = p[BIN(g, t)];
            if (r->hop >= 32) {  // the sync word starting 32 hops back is whole
                uint64_t h0 = r->hop - 32, t0 = h0 * FSKV_HOP;
                double mt = rx_metric(r, h0);
                // One frame waits at a time: a stronger sync word replaces it.
                if (mt >= THRESH && mt > r->peak) {
                    r->peak = mt;
                    r->peak_at = t0;
                }
            }
            r->hop++;
            // Decoded once no stronger peak can come and the whole frame is in.
            if (r->peak > 0 && r->hop * FSKV_HOP > r->peak_at + (KS + 1) * FSKV_SPS + FSKV_WIN &&
                r->peak_at + FSKV_SPAN <= end && r->peak_at >= r->base + 256) {
                r->st.macs = 0;
                int ok = fskv_decode_at(r->buf, r->len, (double)(r->peak_at - r->base), v, &r->st);
                r->st.t0 += (double)r->base;
                r->peak = 0;
                if (ok) return 1;
            }
        }
    }
    return 0;
}
