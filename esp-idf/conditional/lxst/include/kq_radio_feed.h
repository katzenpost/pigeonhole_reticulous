// SPDX-License-Identifier: AGPL-3.0-only
// One radio burst played through the lxst session: its Ogg Opus packets as LXST frames in
// LXST_OP_FRAMES messages, released no more than kLeadMs of audio ahead of where playout has got,
// since lxst holds about 5 s of 20 ms frames per speaker; then LXST_OP_TALK_END.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "kq_ogg.h"

namespace kq::radio {

// As lxst.h and lxst_frame.h, which need spangap; kq_radio.cpp checks they agree.
constexpr uint8_t kOpFrames = 0x02, kOpTalkEnd = 0x03, kCodecOpus = 0x01, kEvBatch = 0x81;
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

// The most one burst of ours lasts: the captain's cap. Past it the burst is sent as if the talk
// key had been let go.
constexpr uint32_t kTalkMaxMs = 120000;

// The time left to talk, from when the talk key went down: what the Radio screen's bar shows.
struct TalkClock {
    uint32_t since = 0;
    bool on = false;
    void start(uint32_t now_ms) { since = now_ms; on = true; }
    void stop() { on = false; }
    uint32_t left_ms(uint32_t now_ms) const {
        uint32_t used = now_ms - since;
        return !on || used >= kTalkMaxMs ? 0 : kTalkMaxMs - used;
    }
    bool expired(uint32_t now_ms) const { return on && left_ms(now_ms) == 0; }
};

// One burst of ours: the frames lxst hands out in LXST_EV_BATCH messages while the talk key is
// held, as an Ogg Opus clip named as hushphone names one, <sha256>.opus. Its frames are heap
// allocations, which on the device's build all go to PSRAM first (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 0).
class Recorder {
public:
    static constexpr uint32_t kMaxMs = kTalkMaxMs;  // a burst is cut here
    // An LXST_EV_BATCH message; false once the burst is full (it keeps what fits), or for a
    // frame that is not Opus.
    bool batch(const uint8_t* m, size_t n);
    bool full() const { return full_; }
    uint32_t ms() const { return (uint32_t)(us_ / 1000); }
    size_t frames() const { return o_.packets.size(); }
    bool empty() const { return o_.packets.empty(); }
    // The clip and its name; the recorder is empty again.
    std::pair<Bytes, std::string> finish(uint32_t serial);

private:
    ogg::Opus o_{1, 312, 16000, {}};  // lxst encodes mono at 16 kHz; libopus's lookahead at 48 kHz
    uint64_t us_ = 0;
    bool full_ = false;
};

}  // namespace kq::radio
