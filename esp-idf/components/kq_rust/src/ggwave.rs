// SPDX-License-Identifier: AGPL-3.0-only
//! Voucher codes by sound through the ggwave crate, as docs/sound-vouchers.md sets out: the payload
//! in ggwave's variable-length mode with Audible Fast, as 16 kHz PCM16. ggwave runs at 24 kHz in
//! 512-sample frames, the bins and frame times of its 48 kHz default; 16 <-> 24 kHz is ours, as
//! ggwave's own resampler is in double, which the S3 does in software. kq_ggwave.h is the C++ face.
use alloc::boxed::Box;
use alloc::vec::Vec;
use core::f64::consts::PI;
use core::ffi::c_int;
use core::slice;

use ggwave::{Decoder, Encoder, Parameters, Protocol};

const INNER: f32 = 24000.0; // ggwave's rate here: 512-sample frames are its 46.875 Hz bins
const FRAME: usize = 512;
const PROTOCOL: Protocol = Protocol::AudibleFast;
const TAPS: usize = 144; // at 48 kHz: flat to 6.4 kHz, the protocol's top tone; -70 dB past 8.3 kHz
const HIST: usize = 145; // an output at up 1 reaches back TAPS inputs
const BLOCK: usize = 320;
// The decoder's recording, allocated when it hears a start marker: the crate's 2048 frames.
const RECORDED: usize = 2048 * FRAME * 4;
// What encoding holds at once for the longest message, 434 frames, 1.2 MB (a voucher: 0.46 MB).
const ENCODING: usize = 5 << 18;

// Running out of memory inside ggwave resets the device, so the memory is asked for first and
// handed back: no room, and the C++ face's "no memory" path is taken instead.
// shortcut: the memory can still go between this and ggwave's own allocation; ggwave-rs with
// fallible allocation (or a smaller recording, as the patched C++ had) is the real fix.
fn room(bytes: usize) -> bool {
    let Ok(l) = core::alloc::Layout::from_size_align(bytes, 4) else { return false };
    let p = unsafe { alloc::alloc::alloc(l) };
    if p.is_null() {
        return false;
    }
    unsafe { alloc::alloc::dealloc(p, l) };
    true
}

fn params() -> Parameters {
    Parameters {
        sample_rate_inp: INNER,
        sample_rate_out: INNER,
        sample_rate: INNER,
        samples_per_frame: FRAME,
        ..Parameters::default()
    }
}

// A low-pass at 48 kHz (7.4 kHz), Blackman-windowed, in double as the C++ had it.
fn lowpass() -> [f32; TAPS] {
    let fc = 7400.0 / 48000.0;
    let mid = (TAPS - 1) as f64 / 2.0;
    let mut h = [0f32; TAPS];
    let mut sum = 0f64;
    for (k, c) in h.iter_mut().enumerate() {
        let t = k as f64 - mid;
        let w = 2.0 * PI * k as f64 / (TAPS - 1) as f64;
        let s = if t == 0.0 { 2.0 * fc } else { libm::sin(2.0 * PI * fc * t) / (PI * t) };
        *c = (s * (0.42 - 0.5 * libm::cos(w) + 0.08 * libm::cos(2.0 * w))) as f32;
        sum += *c as f64;
    }
    for c in &mut h {
        *c = (*c as f64 / sum) as f32;
    }
    h
}

/// A polyphase resampler by up/down through the low-pass: 16 <-> 24 kHz, and 16 <-> 48 kHz for
/// the tests.
pub struct Resampler {
    up: i64,
    down: i64,
    taken: i64, // inputs taken
    next: i64,  // the next output's index at 48 kHz
    hist: [f32; HIST],
    h: [f32; TAPS],
}

impl Resampler {
    pub fn new(up: i64, down: i64) -> Self {
        Resampler { up, down, taken: 0, next: 0, hist: [0.0; HIST], h: lowpass() }
    }

    /// x in; the outputs they complete, at most x.len() * up / down + 1, into y.
    pub fn run(&mut self, x: &[f32], y: &mut [f32]) -> usize {
        let mut m = 0;
        for &s in x {
            self.hist[(self.taken % HIST as i64) as usize] = s;
            self.taken += 1;
            while self.next / self.up < self.taken {
                let mut i = self.next / self.up;
                let mut k = self.next - i * self.up;
                let mut acc = 0f32;
                while k < TAPS as i64 && i >= 0 {
                    acc += self.h[k as usize] * self.hist[(i % HIST as i64) as usize];
                    k += self.up;
                    i -= 1;
                }
                y[m] = acc * self.up as f32;
                m += 1;
                self.next += self.down;
            }
        }
        m
    }
}

/// The payload's sound at 16 kHz; None if ggwave refuses it or there is no memory.
pub fn encode(p: &[u8], volume: c_int) -> Option<Vec<i16>> {
    let volume = u8::try_from(volume).ok()?;
    if !room(ENCODING) {
        return None;
    }
    let w = Encoder::new(params()).ok()?.encode(p, PROTOCOL, volume).ok()?;
    let nw = w.len();
    let total = nw + 64; // and zeros, for the filter's tail
    let mut down = Resampler::new(2, 3);
    let mut out = Vec::with_capacity(total * 2 / 3 + 1);
    let (mut x, mut y) = ([0f32; BLOCK], [0f32; BLOCK]);
    for at in (0..total).step_by(BLOCK) {
        let b = BLOCK.min(total - at);
        for (i, v) in x[..b].iter_mut().enumerate() {
            // as C ggwave's 16-bit output, which the C++ resampled
            *v = w.get(at + i).map_or(0.0, |&s| (32768.0 * s) as i32 as i16 as f32 / 32768.0);
        }
        let ny = down.run(&x[..b], &mut y);
        out.extend(y[..ny].iter().map(|&s| libm::rintf((s * 32768.0).clamp(-32768.0, 32767.0)) as i16));
    }
    Some(out)
}

/// The receiver, fed the microphone as it comes, in blocks of any size.
pub struct Rx {
    gg: Decoder,
    up: Resampler,
}

impl Rx {
    /// None if there is not the memory to listen: the decoder takes its recording only when it
    /// hears a start marker, so it must be there now.
    pub fn new() -> Option<Self> {
        if !room(RECORDED) {
            return None;
        }
        Some(Rx { gg: Decoder::with_protocols(params(), &[PROTOCOL]).ok()?, up: Resampler::new(3, 2) })
    }

    /// Takes x; the length of a payload they completed, copied into out, else 0. failed counts the
    /// transmissions heard whose payload did not decode.
    pub fn push(&mut self, x: &[i16], out: &mut [u8], failed: &mut c_int) -> usize {
        let (mut f, mut y) = ([0f32; BLOCK], [0f32; BLOCK * 3 / 2 + 1]);
        let mut got = 0;
        // A block's 480 samples at 24 kHz complete at most one frame, so a transmission that
        // ends without a payload shows as one push.
        for b in x.chunks(BLOCK) {
            for (v, &s) in f.iter_mut().zip(b) {
                *v = s as f32 / 32768.0;
            }
            let ny = self.up.run(&f[..b.len()], &mut y);
            let was = self.gg.is_receiving();
            let d = self.gg.push(&y[..ny]);
            if was && d.is_none() && !self.gg.is_receiving() {
                *failed += 1;
            }
            if let Some(d) = d.filter(|d| got == 0 && d.len() <= out.len()) {
                out[..d.len()].copy_from_slice(&d);
                got = d.len();
            }
        }
        got
    }
}

// The C ABI, include/kq_rust.h. Null and zero lengths are taken as nothing.

unsafe fn input<'a, T>(p: *const T, n: usize) -> &'a [T] {
    if p.is_null() || n == 0 { &[] } else { unsafe { slice::from_raw_parts(p, n) } }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn kq_gg_encode(p: *const u8, n: usize, volume: c_int, len: *mut usize) -> *mut i16 {
    let w = encode(unsafe { input(p, n) }, volume).unwrap_or_default().into_boxed_slice();
    unsafe { *len = w.len() };
    if w.is_empty() { core::ptr::null_mut() } else { Box::into_raw(w) as *mut i16 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn kq_gg_free(w: *mut i16, len: usize) {
    if !w.is_null() {
        drop(unsafe { Box::from_raw(core::ptr::slice_from_raw_parts_mut(w, len)) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn kq_gg_rx_new() -> *mut Rx {
    Rx::new().map_or(core::ptr::null_mut(), |rx| Box::into_raw(Box::new(rx)))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn kq_gg_rx_free(rx: *mut Rx) {
    if !rx.is_null() {
        drop(unsafe { Box::from_raw(rx) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn kq_gg_rx_push(
    rx: *mut Rx,
    x: *const i16,
    n: usize,
    out: *mut u8,
    cap: usize,
    failed: *mut c_int,
) -> c_int {
    let (Some(rx), Some(failed)) = (unsafe { rx.as_mut() }, unsafe { failed.as_mut() }) else { return 0 };
    let out = if out.is_null() { &mut [][..] } else { unsafe { slice::from_raw_parts_mut(out, cap) } };
    rx.push(unsafe { input(x, n) }, out, failed) as c_int
}

#[unsafe(no_mangle)]
pub extern "C" fn kq_gg_resampler_new(up: c_int, down: c_int) -> *mut Resampler {
    if up < 1 || down < 1 {
        return core::ptr::null_mut();
    }
    Box::into_raw(Box::new(Resampler::new(up as i64, down as i64)))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn kq_gg_resampler_free(r: *mut Resampler) {
    if !r.is_null() {
        drop(unsafe { Box::from_raw(r) });
    }
}

/// y must hold n * up / down + 1 samples.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kq_gg_resampler_run(r: *mut Resampler, x: *const f32, n: usize, y: *mut f32) -> usize {
    let Some(r) = (unsafe { r.as_mut() }) else { return 0 };
    let cap = n * r.up as usize / r.down as usize + 1;
    r.run(unsafe { input(x, n) }, unsafe { slice::from_raw_parts_mut(y, cap) })
}

#[cfg(test)]
mod tests {
    use super::*;

    const VOUCHER: [u8; 32] = [0x5a; 32];

    fn listen(x: &[i16]) -> Option<Vec<u8>> {
        let mut rx = Rx::new()?;
        let (mut out, mut failed) = ([0u8; 32], 0);
        x.chunks(BLOCK).find_map(|b| match rx.push(b, &mut out, &mut failed) {
            0 => None,
            n => Some(out[..n].to_vec()),
        })
    }

    fn padded(x: &[i16]) -> Vec<i16> {
        let mut v = alloc::vec![0i16; 3200];
        v.extend_from_slice(x);
        v.resize(v.len() + 8000, 0);
        v
    }

    // Both ways against ggwave as another program (hushphone) runs it: 48 kHz, 1024-sample frames.
    #[test]
    fn ggwave_at_its_defaults() {
        let tx = encode(&VOUCHER, 25).unwrap();
        let mut up = Resampler::new(3, 1);
        let x: Vec<f32> = padded(&tx).iter().map(|&s| s as f32 / 32768.0).collect();
        let mut y = alloc::vec![0f32; x.len() * 3 + 1];
        let ny = up.run(&x, &mut y);
        let mut theirs = Decoder::with_protocols(Parameters::default(), &[PROTOCOL]).unwrap();
        let heard = y[..ny].chunks(1024).find_map(|f| theirs.push(f));
        assert_eq!(heard.as_deref(), Some(&VOUCHER[..]), "ggwave at 48 kHz hears our voucher");

        let w = Encoder::new(Parameters::default()).unwrap().encode(&VOUCHER, PROTOCOL, 25).unwrap();
        let mut down = Resampler::new(1, 3);
        let mut d = alloc::vec![0f32; w.len() / 3 + 2];
        let nd = down.run(&w, &mut d);
        let pcm: Vec<i16> = d[..nd].iter().map(|&s| libm::rintf(s * 32767.0) as i16).collect();
        assert_eq!(listen(&padded(&pcm)).as_deref(), Some(&VOUCHER[..]), "ours hears ggwave's 48 kHz voucher");
    }

    #[test]
    fn refused() {
        assert!(encode(&[], 25).is_none());
        assert!(encode(&[0; 141], 25).is_none());
        assert!(encode(&VOUCHER, 101).is_none());
        assert!(encode(&VOUCHER, -1).is_none());
    }
}
