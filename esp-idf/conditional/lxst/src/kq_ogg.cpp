// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_ogg.h"

#include <algorithm>
#include <cstring>

namespace kq::ogg {

// Ogg's CRC-32: polynomial 0x04c11db7, not reflected, no initial or final xor.
static uint32_t crc(const uint8_t* p, size_t n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; k++) r = r & 0x80000000u ? r << 1 ^ 0x04c11db7u : r << 1;
            t[i] = r;
        }
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) c = c << 8 ^ t[(c >> 24) ^ p[i]];
    return c;
}

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

std::optional<Opus> demux(const uint8_t* b, size_t n) {
    std::vector<Bytes> packets;
    Bytes part;
    std::optional<uint32_t> serial;
    for (size_t at = 0; at < n;) {
        if (n - at < 27 || memcmp(b + at, "OggS", 4) != 0 || b[at + 4] != 0) return std::nullopt;
        const uint8_t* h = b + at;
        size_t segs = h[26], len = 27 + segs;
        if (n - at < len) return std::nullopt;
        for (size_t i = 0; i < segs; i++) len += h[27 + i];
        if (n - at < len) return std::nullopt;
        Bytes page(h, h + len);
        memset(page.data() + 22, 0, 4);
        if (crc(page.data(), len) != le32(h + 22)) return std::nullopt;
        if (!serial) serial = le32(h + 14);
        if (le32(h + 14) != *serial) return std::nullopt;  // one logical stream only
        if (!(h[5] & 1)) part.clear();                       // not continued: a stale partial packet goes
        const uint8_t* d = h + 27 + segs;
        for (size_t i = 0; i < segs; i++) {
            part.insert(part.end(), d, d + h[27 + i]);
            d += h[27 + i];
            if (h[27 + i] < 255) packets.push_back(std::move(part)), part.clear();
        }
        at += len;
    }
    if (!part.empty() || packets.size() < 2) return std::nullopt;
    const Bytes& head = packets[0];
    if (head.size() < 19 || memcmp(head.data(), "OpusHead", 8) != 0 || head[9] < 1) return std::nullopt;
    if (memcmp(packets[1].data(), "OpusTags", std::min<size_t>(8, packets[1].size())) != 0 || packets[1].size() < 8)
        return std::nullopt;
    Opus o;
    o.channels = head[9];
    o.pre_skip = (uint16_t)(head[10] | head[11] << 8);
    o.input_rate = le32(head.data() + 12);
    o.packets.assign(std::make_move_iterator(packets.begin() + 2), std::make_move_iterator(packets.end()));
    return o;
}

static void page(Bytes& out, uint8_t type, uint64_t granule, uint32_t serial, uint32_t seq,
                 const std::vector<const Bytes*>& packets) {
    size_t at = out.size();
    out.insert(out.end(), {'O', 'g', 'g', 'S', 0, type});
    for (int i = 0; i < 8; i++) out.push_back((uint8_t)(granule >> (8 * i)));
    for (uint32_t v : {serial, seq, (uint32_t)0})
        for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i)));
    Bytes lacing;
    for (auto* p : packets) {
        for (size_t n = p->size(); ; n -= 255) {
            lacing.push_back((uint8_t)std::min<size_t>(n, 255));
            if (n < 255) break;
        }
    }
    out.push_back((uint8_t)lacing.size());
    out.insert(out.end(), lacing.begin(), lacing.end());
    for (auto* p : packets) out.insert(out.end(), p->begin(), p->end());
    uint32_t c = crc(out.data() + at, out.size() - at);
    for (int i = 0; i < 4; i++) out[at + 22 + i] = (uint8_t)(c >> (8 * i));
}

Bytes Muxer::start() {
    const Opus& o = head_;
    Bytes head{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, o.channels, (uint8_t)o.pre_skip, (uint8_t)(o.pre_skip >> 8)};
    for (int i = 0; i < 4; i++) head.push_back((uint8_t)(o.input_rate >> (8 * i)));
    head.insert(head.end(), {0, 0, 0});  // output gain 0, mapping family 0
    static const char vendor[] = "kq";
    Bytes tags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's', sizeof vendor - 1, 0, 0, 0, 'k', 'q', 0, 0, 0, 0};
    Bytes out;
    page(out, 2, 0, serial_, seq_++, {&head});
    page(out, 0, 0, serial_, seq_++, {&tags});
    return out;
}

Bytes Muxer::add(const Bytes& packet) {
    Bytes out;
    size_t segs = packet.size() / 255 + 1;
    // A page holds at most 255 lacing values; 50 packets of 20 ms make one second per page.
    if (page_.size() == 50 || segs_ + segs > 255) {
        std::vector<const Bytes*> ps;
        for (auto& q : page_) ps.push_back(&q);
        page(out, 0, granule_, serial_, seq_++, ps);
        page_.clear();
        segs_ = 0;
    }
    page_.push_back(packet);
    segs_ += segs;
    granule_ += packet_us(packet) * 48 / 1000;
    return out;
}

Bytes Muxer::finish() {
    Bytes out;
    if (page_.empty()) return out;
    std::vector<const Bytes*> ps;
    for (auto& q : page_) ps.push_back(&q);
    page(out, 4, granule_, serial_, seq_++, ps);
    page_.clear();
    segs_ = 0;
    return out;
}

Bytes mux(const Opus& o, uint32_t serial) {
    Muxer m(o, serial);
    Bytes out = m.start();
    for (auto& p : o.packets) {
        Bytes pg = m.add(p);
        out.insert(out.end(), pg.begin(), pg.end());
    }
    Bytes last = m.finish();
    out.insert(out.end(), last.begin(), last.end());
    return out;
}

uint32_t packet_us(const Bytes& p) {
    if (p.empty()) return 0;
    int c = p[0] >> 3;
    static const uint32_t silk[] = {10000, 20000, 40000, 60000}, celt[] = {2500, 5000, 10000, 20000};
    uint32_t frame = c < 12 ? silk[c % 4] : c < 16 ? (c % 2 ? 20000 : 10000) : celt[c % 4];
    switch (p[0] & 3) {
        case 0: return frame;
        case 1: case 2: return 2 * frame;
        default: return p.size() < 2 ? 0 : frame * (p[1] & 0x3f);
    }
}

}  // namespace kq::ogg
