// SPDX-License-Identifier: AGPL-3.0-only
// The acoustic voucher modem: 32 bytes, framed [len][voucher][crc16] and RS(51,35) coded, sent as
// MFSK in the voice band at 16 kHz: 4 groups of 16 tones at once, 62.5 Hz apart, 1000-4938 Hz,
// 25 symbols a second, 1.2 s a frame. Plain C, no allocation.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSKV_FS 16000
#define FSKV_PAYLOAD 32
#define FSKV_FRAME (1 + FSKV_PAYLOAD + 2)  // len, voucher, crc16
#define FSKV_NSYM 16                        // RS parity bytes: corrects 8 byte errors
#define FSKV_CW (FSKV_FRAME + FSKV_NSYM)    // 51
#define FSKV_SPS 640                        // samples a symbol: a 32 ms window behind an 8 ms echo guard
#define FSKV_WIN 512
#define FSKV_SYMBOLS (4 + 26)               // sync, then the coded bytes at 16 bits a symbol
#define FSKV_SAMPLES (FSKV_SYMBOLS * FSKV_SPS)  // 19200
#define FSKV_TONES 64

// Renders the frame at peak amplitude amp (<= 32767) into out[FSKV_SAMPLES].
void fskv_modulate(const uint8_t v[FSKV_PAYLOAD], float amp, int16_t* out);

typedef struct {
    double t0, a;      // where the frame was found, the clock ratio (RX samples per TX sample) it decoded at
    double metric;     // its sync metric: about 1 clean, 0 on noise
    int rs_fixed;      // byte errors RS corrected, -1 if it failed
    int tries;         // clock ratios handed to RS
    long macs;         // complex multiply-accumulates spent
} fskv_stats;

// Finds and decodes one frame anywhere in x. 1 with v filled, 0 if none decoded.
int fskv_decode(const int16_t* x, size_t n, uint8_t v[FSKV_PAYLOAD], fskv_stats* st);
// Decodes the frame whose sync was found near t (at the nominal clock). 1 with v filled.
int fskv_decode_at(const int16_t* x, size_t n, double t, uint8_t v[FSKV_PAYLOAD], fskv_stats* st);
// Hard bits of the coded frame (FSKV_CW*8) demodulated at t0, a: for genie-timed raw BER.
void fskv_demod_bits(const int16_t* x, size_t n, double t0, double a, uint8_t* bits);
// Best sync metric and t0 within [lo, hi] at clock ratio a.
double fskv_refine(const int16_t* x, size_t n, double lo, double hi, double step, double a, double* t0);
// Samples after a frame's start that fskv_decode_at may read.
#define FSKV_SPAN ((size_t)(FSKV_SAMPLES * 1.007) + 2 * FSKV_WIN)

// The receiver, fed the microphone as it comes. It keeps the recent audio in buf (cap samples,
// at least FSKV_RX_MIN) and one FFT of it every 64 samples, and decodes where a sync word shows.
#define FSKV_HOP 64
#define FSKV_HIST 64  // hops of tone energies kept: the sync word spans 33
#define FSKV_RX_MIN (FSKV_SPAN + 8 * FSKV_SPS)
typedef struct {
    int16_t* buf;
    size_t cap, len;
    uint64_t base;               // stream index of buf[0]
    uint64_t hop;                // the next hop to transform: window [hop*64, hop*64 + 512)
    float e[FSKV_HIST][FSKV_TONES];
    double peak;                 // best sync metric of the frame waiting to be decoded, 0 if none
    uint64_t peak_at;            // where it starts, as a stream index
    fskv_stats st;               // of the last decode tried
    long ffts;
} fskv_rx;
void fskv_rx_init(fskv_rx* r, int16_t* buf, size_t cap);
// Takes n samples; 1 when a frame decoded from them, with v filled.
int fskv_rx_push(fskv_rx* r, const int16_t* x, size_t n, uint8_t v[FSKV_PAYLOAD]);

// Pieces, exposed for tests.
uint16_t fskv_crc16(const uint8_t* p, size_t n);
void fskv_rs_encode(const uint8_t* msg, size_t k, uint8_t* cw);  // cw = msg || FSKV_NSYM parity
int fskv_rs_decode(uint8_t* cw, size_t n);                        // errors fixed, -1 if uncorrectable
void fskv_frame(const uint8_t v[FSKV_PAYLOAD], uint8_t cw[FSKV_CW]);
// |X[k]|^2 of the 512-point DFT of x for k in [0, 256).
void fskv_spectrum(const int16_t* x, float* p);

#ifdef __cplusplus
}
#endif
