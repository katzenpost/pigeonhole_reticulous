// SPDX-License-Identifier: AGPL-3.0-only
// A room between two devices, for the sound voucher tests: clock drift, a small speaker's and a
// MEMS mic's band, reverb, attenuation to -30 dBFS, white noise, int16 capture.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* name;
    double rt60, drr_db;  // reverb tail; direct-to-reverberant ratio. rt60 0: none
    int band;             // speaker/mic band limit and ripple
    double early;         // largest of six early reflections (1-15 ms), relative to the direct path
} room_chan;
// Two devices 20-50 cm apart on a table indoors; a bare, echoing room; band limit and noise only.
extern const room_chan ROOM_TABLE, ROOM_STRESS, ROOM_DRY;

void room_seed(uint64_t s);
uint64_t room_rand(void);
double room_uniform(void);
double room_gauss(void);
// tx after lead samples of silence and before tail, through the room: drift stretches it by
// 1 + drift, the frame comes out at -30 dBFS RMS with noise snr_db below. Returns rx's length
// (0 if cap is short); *t0 is where the frame starts in rx.
size_t room_channel(const int16_t* tx, size_t ntx, size_t lead, size_t tail, const room_chan* c, double drift,
                    double snr_db, int16_t* rx, size_t cap, double* t0);
// The noise room_channel adds at snr_db, as int16 samples.
void room_noise(double snr_db, int16_t* x, size_t n);

int wav_write(const char* path, const int16_t* x, size_t n);
size_t wav_read(const char* path, int16_t* x, size_t cap);  // 16 kHz mono PCM16; 0 if not

#ifdef __cplusplus
}
#endif
