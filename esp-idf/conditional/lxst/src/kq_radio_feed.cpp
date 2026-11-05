// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_radio_feed.h"

namespace kq::radio {

static void le(Bytes& b, uint32_t v, int n) {
    for (int i = 0; i < n; i++) b.push_back((uint8_t)(v >> (8 * i)));
}

bool Feed::start(uint32_t speaker, const Bytes& ogg, uint32_t now_ms) {
    auto o = ogg::demux(ogg.data(), ogg.size());
    if (!o || o->packets.empty()) return false;
    packets_ = std::move(o->packets);
    total_us_ = 0;
    for (auto& p : packets_) total_us_ += ogg::packet_us(p);
    next_ = 0;
    sent_us_ = 0;
    speaker_ = speaker;
    start_ms_ = now_ms;
    seq_ = 0;
    active_ = true;
    return true;
}

std::vector<Bytes> Feed::due(uint32_t now_ms) {
    std::vector<Bytes> out;
    if (!active_) return out;
    uint64_t until = (uint64_t)(now_ms - start_ms_ + kLeadMs) * 1000;
    while (next_ < packets_.size() && sent_us_ < until) {
        Bytes m{kOpFrames};
        le(m, speaker_, 4);
        le(m, seq_, 2);
        m.push_back(0);
        size_t count = 0;
        while (next_ < packets_.size() && sent_us_ < until && count < kMaxBatch) {
            const Bytes& p = packets_[next_];
            if (count && m.size() + 3 + p.size() > kMaxMessage) break;
            le(m, (uint32_t)p.size() + 1, 2);
            m.push_back(kCodecOpus);
            m.insert(m.end(), p.begin(), p.end());
            sent_us_ += ogg::packet_us(p);
            count++;
            next_++;
        }
        m[7] = (uint8_t)count;
        seq_ = (uint16_t)(seq_ + count);
        out.push_back(std::move(m));
    }
    if (next_ == packets_.size()) {
        Bytes end{kOpTalkEnd};
        le(end, speaker_, 4);
        out.push_back(std::move(end));
        active_ = false;
        packets_.clear();
    }
    return out;
}

}  // namespace kq::radio
