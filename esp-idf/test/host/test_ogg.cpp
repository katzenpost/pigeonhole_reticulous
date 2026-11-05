// SPDX-License-Identifier: AGPL-3.0-only
// The Ogg Opus demuxer on hushphone's test clip (hushphone tests/data/tone.opus): a 1 s tone,
// 12 kbit/s CBR, 20 ms packets, as rustic_audio's encoder writes it.
#include <fstream>
#include <iterator>

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
    return finish("ogg");
}
