// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_ogg.h"

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

}  // namespace kq::ogg
