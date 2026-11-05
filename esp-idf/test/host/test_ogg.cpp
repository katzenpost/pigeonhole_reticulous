// SPDX-License-Identifier: AGPL-3.0-only
// The Ogg Opus demuxer on hushphone's test clip (hushphone tests/data/tone.opus): a 1 s tone,
// 12 kbit/s CBR, 20 ms packets, as rustic_audio's encoder writes it.
#include <fstream>
#include <iterator>

#include "kq_ogg.h"
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
    return finish("ogg");
}
