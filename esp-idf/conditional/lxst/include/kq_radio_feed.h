// SPDX-License-Identifier: AGPL-3.0-only
// One radio burst played through the lxst session: its Ogg Opus packets as LXST frames in
// LXST_OP_FRAMES messages, released no more than kLeadMs of audio ahead of where playout has got,
// since lxst holds about 5 s of 20 ms frames per speaker; then LXST_OP_TALK_END.
#pragma once
#include <vector>

#include "kq_ogg.h"

namespace kq::radio {

// As lxst.h and lxst_frame.h, which need spangap; kq_radio.cpp checks they agree.
constexpr uint8_t kOpFrames = 0x02, kOpTalkEnd = 0x03, kCodecOpus = 0x01;
constexpr size_t kMaxBatch = 64, kMaxMessage = 4096;

class Feed {
public:
    static constexpr uint32_t kLeadMs = 4000;

    // false when ogg is not Ogg Opus with audio in it.
    bool start(uint32_t speaker, const Bytes& ogg, uint32_t now_ms);
    // What is due by now: frames messages, and once they are all out, the talk end.
    std::vector<Bytes> due(uint32_t now_ms);
    bool active() const { return active_; }
    uint32_t duration_ms() const { return (uint32_t)(total_us_ / 1000); }
    uint32_t speaker() const { return speaker_; }

private:
    std::vector<Bytes> packets_;
    size_t next_ = 0;
    uint64_t sent_us_ = 0, total_us_ = 0;
    uint32_t speaker_ = 0, start_ms_ = 0;
    uint16_t seq_ = 0;
    bool active_ = false;
};

}  // namespace kq::radio
