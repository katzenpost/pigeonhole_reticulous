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

The audio engine runs at 16 kHz. `components/kq_ggwave` runs ggwave at 24 kHz with 512-sample
frames and resamples 16 <-> 24 kHz itself, through a 144-tap low-pass at 48 kHz: ggwave's own
resampler works in double precision, which the ESP32-S3 does in software. The level is
`s.kq.sound_volume` (25 unless set).

The copy of ggwave there differs from upstream only where it is marked `kq`:

- `kMaxRecordedFrames` is 448, not 2048. That takes ggwave's own memory from 4.2 MB to 0.94 MB
  for the receiver, and from 4.6 MB to 1.3 MB for the sender (16-bit output), all PSRAM.
- The receiver records at most that many frames.
- The sender refuses a message longer than that.
- `prepare()` fails when its allocation does.

None of this changes what goes over the air, and 448 frames hold the longest Audible Fast message
(140 bytes, 434 frames).

## How well it works, simulated

`esp-idf/test/host/test_ggwave` checks:

- Both directions against ggwave at its 48 kHz defaults, through its C API.
- The fixtures in `vectors/ggwave`.
- Vouchers through a simulated room: a small speaker's and a MEMS mic's band, reverb, noise and
  clock drift.

In the ordinary room no voucher is lost at 0 dB SNR and up.

ggwave does not track clock drift. Vouchers get through at ±0.1% between the two clocks and are
all lost at ±0.2%. Crystal clocks are within 0.01%. It is not yet tested on hardware.
