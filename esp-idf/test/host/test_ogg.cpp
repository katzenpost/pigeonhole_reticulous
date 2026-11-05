// SPDX-License-Identifier: AGPL-3.0-only
// The Ogg Opus demuxer on hushphone's test clip (hushphone tests/data/tone.opus): a 1 s tone,
// 12 kbit/s CBR, 20 ms packets, as rustic_audio's encoder writes it.
#include <fstream>
#include <iterator>

#include <sodium.h>

#include "kq1_wire.h"
#include "kq_ogg.h"
#include "kq_radio_feed.h"
#include "vectors.h"

// One Opus frame's length from its TOC byte (RFC 6716 3.1), in tenths of a millisecond.
static int frame_ms10(uint8_t toc) {
    int c = toc >> 3;
    static const int silk[] = {100, 200, 400, 600}, celt[] = {25, 50, 100, 200};
    return c < 12 ? silk[c % 4] : c < 16 ? (c % 2 ? 200 : 100) : celt[c % 4];
}

int main(int, char**) {
    Vec v{"ogg", "tone.opus", {}};
    std::ifstream in("vectors/hushphone/tone.opus", std::ios::binary);
    kq::Bytes clip((std::istreambuf_iterator<char>(in)), {});
    check(clip.size() == 1668, v, "the clip");
    auto o = kq::ogg::demux(clip.data(), clip.size());
    check(o && o->channels == 1 && o->pre_skip == 0 && o->input_rate == 48000, v, "its OpusHead");
    size_t bytes = 0;
    bool cbr = o.has_value();
    if (o)
        for (auto& p : o->packets) {
            bytes += p.size();
            cbr &= p.size() == 30 && frame_ms10(p[0]) == 200 && (p[0] & 3) == 0;
        }
    check(o && o->packets.size() == 50 && bytes == 1500, v, "50 packets, 1500 bytes of audio");
    check(cbr, v, "each one 20 ms frame of 30 bytes");

    kq::Bytes bad = clip;
    bad[200] ^= 1;
    check(!kq::ogg::demux(bad.data(), bad.size()), v, "a changed byte fails its page's CRC");
    check(!kq::ogg::demux(clip.data(), clip.size() - 1), v, "a clip cut short");
    check(!kq::ogg::demux(clip.data(), 0), v, "nothing");
    kq::Bytes junk(100, 'x');
    check(!kq::ogg::demux(junk.data(), junk.size()), v, "not Ogg");
    check(kq::ogg::packet_us(o->packets[0]) == 20000, v, "a packet's length from its TOC byte");

    // Muxed again, it demuxes to the same packets.
    auto again = kq::ogg::demux(kq::ogg::mux(*o, 7).data(), kq::ogg::mux(*o, 7).size());
    check(again && again->packets == o->packets && again->input_rate == 48000 && again->channels == 1, v,
          "mux, then demux");

    // The feed: tone.opus is out at once, frames codec byte first, then the talk end.
    using namespace kq::radio;
    auto frames_of = [](const kq::Bytes& m, uint16_t& seq) {
        std::vector<kq::Bytes> fs;
        if (m.size() < 8 || m[0] != kOpFrames) return fs;
        seq = (uint16_t)(m[5] | m[6] << 8);
        size_t at = 8;
        for (size_t i = 0; i < m[7] && at + 2 <= m.size(); i++) {
            size_t n = m[at] | m[at + 1] << 8;
            fs.emplace_back(m.begin() + at + 2, m.begin() + at + 2 + n);
            at += 2 + n;
        }
        return at == m.size() ? fs : std::vector<kq::Bytes>{};
    };
    Feed f;
    check(!f.start(1, junk, 0) && f.start(0x01020304, clip, 1000) && f.duration_ms() == 1000, v, "a feed starts");
    auto ms = f.due(1000);
    uint16_t seq = 99;
    auto fs = ms.size() == 2 ? frames_of(ms[0], seq) : std::vector<kq::Bytes>{};
    bool same = fs.size() == 50 && seq == 0 && ms[0][1] == 4 && ms[0][4] == 1;
    for (size_t i = 0; same && i < fs.size(); i++) same = fs[i][0] == kCodecOpus && kq::Bytes(fs[i].begin() + 1, fs[i].end()) == o->packets[i];
    check(same && ms[1] == kq::Bytes({kOpTalkEnd, 4, 3, 2, 1}) && !f.active(), v, "a 1 s burst in one message");

    // A 12 s burst: 4 s ahead at the start, then as playout moves; sequence numbers run on.
    kq::ogg::Opus long_ = *o;
    for (int k = 0; k < 11; k++) long_.packets.insert(long_.packets.end(), o->packets.begin(), o->packets.end());
    f.start(9, kq::ogg::mux(long_, 1), 0);
    size_t out = 0;
    uint16_t next = 0;
    bool ordered = true, ended = false;
    auto take = [&](uint32_t now) {
        for (auto& m : f.due(now)) {
            if (m[0] == kOpTalkEnd) { ended = true; continue; }
            auto x = frames_of(m, seq);
            ordered &= !x.empty() && x.size() <= kMaxBatch && seq == next && m.size() <= kMaxMessage;
            next = (uint16_t)(next + x.size());
            out += x.size();
        }
    };
    take(0);
    check(out == 200 && !ended, v, "4 s of a 12 s burst at once");
    take(1000);
    check(out == 250 && !ended, v, "then a second's worth a second later");
    take(20000);
    check(out == 600 && ended && ordered && f.duration_ms() == 12000, v, "the rest, in order, then the talk end");

    // Our own burst: lxst's batches (codec byte first) recorded, then muxed and named.
    Recorder rec(5);
    auto batch = [](const std::vector<kq::Bytes>& ps, size_t from, size_t n) {
        kq::Bytes m{kEvBatch, (uint8_t)n};
        for (size_t i = from; i < from + n; i++) {
            m.push_back((uint8_t)(ps[i].size() + 1));
            m.push_back(0);
            m.push_back(kCodecOpus);
            m.insert(m.end(), ps[i].begin(), ps[i].end());
        }
        return m;
    };
    bool all = true;
    // Taken as it is made, as the stream is fed: the head pages at once, then a page a second.
    kq::Bytes burst;
    std::vector<size_t> taken;
    for (size_t k = 0; k < 3; k++)
        for (size_t i = 0; i < 50; i += 10) {
            all &= rec.batch(batch(o->packets, i, 10).data(), batch(o->packets, i, 10).size());
            kq::Bytes t = rec.take();
            taken.push_back(t.size());
            burst.insert(burst.end(), t.begin(), t.end());
        }
    check(all && rec.frames() == 150 && rec.ms() == 3000, v, "three seconds of batches recorded");
    check(taken[0] > 0 && taken[1] == 0 && taken[5] > 0 && taken[10] > 0, v, "the head pages at once, a page each second");
    rec.end();
    kq::Bytes last = rec.take();
    burst.insert(burst.end(), last.begin(), last.end());
    kq::ogg::Opus three{1, 312, 16000, {}};
    for (int k = 0; k < 3; k++) three.packets.insert(three.packets.end(), o->packets.begin(), o->packets.end());
    auto back = kq::ogg::demux(burst.data(), burst.size());
    check(back && back->packets == three.packets && back->input_rate == 16000 && back->pre_skip == 312 &&
              burst == kq::ogg::mux(three, 5), v, "page by page, the burst is what mux makes of it all");
    uint8_t r32[32];
    randombytes_buf(r32, sizeof r32);
    std::string name = burst_name(r32);
    check(name.size() == 69 && name.substr(0, 64) == tohex(kq::Bytes(r32, r32 + 32)) &&
              kq1::filetype_of(name) == std::string("audio/opus"), v, "named as hushphone names a clip");
    rec = Recorder(6);
    kq::Bytes raw = batch(o->packets, 0, 1);
    raw[4] = 0;  // codec byte RAW
    check(!rec.batch(raw.data(), raw.size()), v, "a frame that is not Opus is refused");
    // The captain's cap: a burst holds 120 s; what comes after is refused, what fit is kept.
    bool cut = false;
    for (int k = 0; k < 121 && !cut; k++)
        for (size_t i = 0; i < 50 && !cut; i += 10) cut = !rec.batch(batch(o->packets, i, 10).data(), batch(o->packets, i, 10).size());
    check(cut && rec.full() && rec.ms() == 120000 && rec.frames() == 6000 && kTalkMaxMs == 120000, v,
          "a burst is cut at 120 s with all of it kept");
    check(!rec.batch(batch(o->packets, 0, 1).data(), batch(o->packets, 0, 1).size()), v, "and takes nothing more");
    rec.end();
    kq::Bytes two = rec.take();
    auto two_ = kq::ogg::demux(two.data(), two.size());
    check(two_ && two_->packets.size() == 6000, v, "the 120 s burst muxes whole");

    // The bar: full at key down, draining with the clock, empty at 120 s, nothing when not talking.
    TalkClock tc;
    check(tc.left_ms(5) == 0 && !tc.expired(5), v, "no time left to show when not talking");
    tc.start(1000);
    check(tc.left_ms(1000) == 120000 && tc.left_ms(31000) == 90000 && tc.left_ms(1250) == 119750, v,
          "the time left, to the quarter second");
    check(!tc.expired(120999) && tc.expired(121000) && tc.left_ms(500000) == 0, v, "expired at 120 s");
    tc.start(0xFFFFFF00u);
    check(tc.left_ms(0x00000100u) == 120000 - 0x200, v, "across the millisecond clock's wrap");
    tc.stop();
    check(tc.left_ms(0x00000100u) == 0, v, "stopped");
    if (const char* out = getenv("KQ_OGG_OUT")) {
        FILE* f = fopen(out, "wb");
        if (f) { fwrite(burst.data(), 1, burst.size(), f); fclose(f); }
    }
    return finish("ogg");
}
