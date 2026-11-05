// SPDX-License-Identifier: AGPL-3.0-only
// Ogg Opus (RFC 7845), as hushphone and katzenqt send voice clips: one logical stream, the
// OpusHead and OpusTags packets, then the audio packets.
#pragma once
#include <optional>

#include "kq_crypto.h"

namespace kq::ogg {

struct Opus {
    uint8_t channels = 1;
    uint16_t pre_skip = 0;
    uint32_t input_rate = 48000;
    std::vector<Bytes> packets;  // the audio, one raw Opus packet each
};

// nullopt when b is not a whole Ogg Opus stream: a bad capture pattern or CRC, a packet cut
// short, or no OpusHead.
std::optional<Opus> demux(const uint8_t* b, size_t n);

// An Ogg Opus stream of these packets: OpusHead and OpusTags pages, then audio pages of up to
// 50 packets, the last marked end of stream, granule positions counted at 48 kHz.
Bytes mux(const Opus& o, uint32_t serial);

// How long one raw Opus packet plays, from its TOC byte (RFC 6716 3.1), in microseconds; 0 for
// an empty packet or a code 3 one without its count byte.
uint32_t packet_us(const Bytes& p);

}  // namespace kq::ogg
