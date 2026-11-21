# Voucher codes by sound

A device that joins a group gives its 32-byte voucher to a member's device by sound, with
[ggwave](https://github.com/ggerganov/ggwave). The T-Deck does it in Group Chat (**Send by
sound** beside the voucher QR, **Listen** in Add member) and in the Radio image. hushphone has
to send and hear exactly this:

| | |
|---|---|
| ggwave | commit `060aec73dd7123ccac200442f75bdc7369795ffe` (MIT) |
| protocol | `GGWAVE_PROTOCOL_AUDIBLE_FAST`, id 1 of `ggwave_ProtocolId` |
| mode | variable length: `payloadLength` -1, ggwave's default, with its start and end markers |
| payload | the voucher's 32 raw bytes, nothing around them; not the base64 code |
| sample rate | any rate whose frames give ggwave's 46.875 Hz bins: its default 48000 Hz with 1024-sample frames, or 24000 Hz with 512 |
| volume | 25 (`ggwave_encode`'s volume, 0-100): peaks at a quarter of full scale, about -25 dBFS RMS |

What that comes to:

- **Tones:** 1875-6328 Hz on the 46.875 Hz grid.
- **Length:** 128 frames of 21.33 ms, 2.73 s in all: 16 start-marker frames, 16 Txs of 6 frames for
  ggwave's 3-byte length, the 32 bytes and its 12 Reed-Solomon bytes, then 16 end-marker frames.
- **Receiving:**
  - Enable only Audible Fast for Rx (`ggwave_rxToggleProtocol`), and accept only a 32-byte payload.
  - Give `ggwave_decode` whole frames, `samplesPerFrame` samples a call: unless it is resampling,
    ggwave drops the part of a frame.
- **Check digits:** both screens show the same 4. They are SHAKE256 of the 32 bytes, 4 bytes out,
  read as a big-endian uint32, mod 10000, zero-padded. The vector's voucher
  (`8so/D+D9qkFMnlXrzZVJXeAXJx8cBqVsihu/hWyubW8=`) gives `1939`.
- **A recording to test with:** `esp-idf/test/host/vectors/ggwave/tx.wav` is that voucher as the
  T-Deck plays it, 16 kHz mono PCM16.

## On the T-Deck

The audio engine runs at 16 kHz. ggwave there is the Rust
[ggwave crate](https://github.com/katzenpost/ggwave-rs) 0.2.0, a port of `060aec7` whose samples
match C ggwave's, in `components/kq_rust` (`src/ggwave.rs`); `components/kq_ggwave` is its C++
face. It runs ggwave at 24 kHz with 512-sample frames and resamples 16 <-> 24 kHz itself, through a
144-tap low-pass at 48 kHz: ggwave's own resampler works in double precision, which the ESP32-S3
does in software. The level is `s.kq.sound_volume` (25 unless set).

What it holds in PSRAM, only during a transfer:

- **Listening: 4.2 MB.** The crate records up to 2048 frames, and takes that memory when it hears
  a start marker. Listening does not start unless that much is free. (The patched C++ copy this
  replaced recorded 448 frames, 0.94 MB.)
- **Sending: 0.46 MB** while a voucher is encoded: the sound at 24 kHz and ggwave's tones, then
  the 16 kHz copy. Sending does not start unless 1.25 MB is free, what the longest message takes.
  (The patched C++ copy took 1.3 MB.)

None of this changes what goes over the air: a voucher's sound is `tx.wav`'s, which the C++ copy
made, sample for sample. The end marker ends a recording, so a message never needs the extra frames.

## How well it works, simulated

`esp-idf/test/host/test_ggwave` checks:

- A voucher's sound against `vectors/ggwave/tx.wav`, sample for sample.
- The fixtures in `vectors/ggwave`.
- Both directions against ggwave at its 48 kHz defaults: the ggwave crate at its defaults, in
  `components/kq_rust`'s `cargo test`.
- Vouchers through a simulated room: a small speaker's and a MEMS mic's band, reverb, noise and
  clock drift.

In the ordinary room no voucher is lost at 0 dB SNR and up.

ggwave does not track clock drift. Vouchers get through at ±0.1% between the two clocks and are
all lost at ±0.2%. Crystal clocks are within 0.01%. It is not yet tested on hardware.
