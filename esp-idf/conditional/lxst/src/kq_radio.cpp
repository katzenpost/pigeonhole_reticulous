// SPDX-License-Identifier: AGPL-3.0-only
// The kq.radio task: takes the bursts the engine hands out for radio groups, holds them in
// PSRAM, and plays them through the lxst session one at a time. It holds the session only while
// there is something to play, since partyline and calls share it.
#include "kq_radio.h"

#include <cstring>
#include <deque>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lxst.h"
#include "spangap.h"

#include "kq1_engine.h"
#include "kq_radio_feed.h"
#include "kq_view.h"

static_assert(kq::radio::kOpFrames == LXST_OP_FRAMES && kq::radio::kOpTalkEnd == LXST_OP_TALK_END &&
                  kq::radio::kCodecOpus == LXST_CODEC_OPUS && kq::radio::kMaxBatch == LXST_MAX_BATCH,
              "kq_radio_feed.h follows lxst.h");

namespace {

constexpr size_t kQueueBytes = 1 << 20;          // bursts waiting, at most; the oldest go first
constexpr uint32_t kStaleMs = 5 * 60 * 1000;     // a burst waiting longer is missed, not played
constexpr uint32_t kHoldMs = 3000;               // the session is let go this long after the last burst
constexpr uint16_t kJitterMs = 400;

struct Burst {
    std::string who;
    uint32_t speaker = 0;
    uint8_t* data = nullptr;  // in PSRAM
    size_t n = 0;
    uint32_t at = 0;          // when it was fetched
};

// s_q and s_st, which the kq task and the CLI touch too. A FreeRTOS mutex, not std::mutex: on
// the Linux image a host mutex held by a task the scheduler has set aside blocks the next one
// outside the scheduler, and the station stops.
SemaphoreHandle_t s_mu = nullptr;
struct Lock {
    Lock() { xSemaphoreTake(s_mu, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mu); }
};
std::deque<Burst> s_q;
size_t s_qBytes = 0;
KqRadio s_st;
TaskHandle_t s_task = nullptr;

int s_lx = -1;
kq::radio::Feed s_feed;
Burst s_cur;                 // what is playing; its data is freed once the feed has it
uint32_t s_started = 0, s_lastEnd = 0;
bool s_heardOne = false;

uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

void release(Burst& b) {
    heap_caps_free(b.data);
    b.data = nullptr;
}

void onLxRecv(int handle, size_t) {
    uint8_t m[16];
    for (size_t n; (n = itsRecv(handle, m, sizeof m, 0)) > 0;) {
        if (handle != s_lx || m[0] != LXST_EV_SPEAKING || n < 6) continue;
        uint32_t sp = m[1] | m[2] << 8 | m[3] << 16 | (uint32_t)m[4] << 24;
        if (sp != s_cur.speaker) continue;
        Lock g;
        if (m[5] && !s_heardOne) {
            s_heardOne = true;
            uint32_t now = nowMs();
            char line[160];
            snprintf(line, sizeof line, "%s, %.1f s: heard %u ms after it was fetched, %u ms after it was queued to play",
                     s_cur.who.c_str(), s_feed.duration_ms() / 1000.0, (unsigned)(now - s_cur.at),
                     (unsigned)(now - s_started));
            s_st.last = line;
            info("kq radio: %s", line);
        }
        if (!m[5]) { s_st.playing.clear(); s_lastEnd = nowMs(); }
    }
}

void onLxDisc(int) { s_lx = -1; }

bool open() {
    if (s_lx >= 0) return true;
    lxst_profile_t p = {};
    p.codec = LXST_CODEC_OPUS;
    p.mode = LXST_OPUS_VOICE_MEDIUM;
    p.frame_ms = 20;
    p.max_frame_bytes = 512;
    p.batch_budget = 1400;
    p.batch_per_frame = 2;
    p.batch_max_frames = LXST_MAX_BATCH;
    p.jitter_ms = kJitterMs;
    p.max_jitter_ms = 30000;
    p.sequenced = 1;
    p.can_speak = 0;  // listen only
    p.tx_mode = LXST_TX_PTT;
    p.rx_any = 1;     // hushphone's 48 kHz 20 ms Opus, a T-Deck's 60 ms: whatever a burst carries
    s_lx = itsConnect(LXST_TASK, LXST_PORT_SESSION, &p, sizeof p, pdMS_TO_TICKS(1000), 0, onLxRecv, onLxDisc);
    if (s_lx < 0) return false;
    uint8_t st[4] = {LXST_OP_STATE, 0, 1, 0};
    itsSend(s_lx, st, sizeof st, 0);
    return true;
}

bool next(Burst& b) {
    Lock g;
    while (!s_q.empty()) {
        b = s_q.front();
        s_q.pop_front();
        s_qBytes -= b.n;
        s_st.queued = (unsigned)s_q.size();
        if (nowMs() - b.at <= kStaleMs) return true;
        release(b);
        s_st.missed++;
    }
    return false;
}

void radioMain(void*) {
    itsClientInit(2);
    for (;;) {
        itsPoll(pdMS_TO_TICKS(40));
        uint32_t now = nowMs();
        if (!s_feed.active() && s_st.playing.empty()) {
            if (s_cur.data == nullptr && next(s_cur)) {
                if (!open()) {
                    warn("kq radio: the voice session is in use; %s's burst is missed", s_cur.who.c_str());
                    release(s_cur);
                    Lock g;
                    s_st.missed++;
                    continue;
                }
                bool ok = s_feed.start(s_cur.speaker, kq::Bytes(s_cur.data, s_cur.data + s_cur.n), now);
                release(s_cur);
                Lock g;
                if (!ok) {
                    warn("kq radio: %s sent a burst that is not Ogg Opus", s_cur.who.c_str());
                    s_st.missed++;
                    continue;
                }
                s_started = now;
                s_heardOne = false;
                s_st.playing = s_cur.who;
                s_st.heard++;
            } else if (s_lx >= 0 && now - s_lastEnd > kHoldMs) {
                itsDisconnect(s_lx);
                s_lx = -1;
            }
        }
        // lxst says when a speaker goes quiet; should it not, the burst is over once it has had its time.
        if (!s_feed.active() && !s_st.playing.empty() && now - s_started > s_feed.duration_ms() + kJitterMs + 5000) {
            Lock g;
            s_st.playing.clear();
            s_lastEnd = now;
        }
        if (s_feed.active() && s_lx >= 0)
            for (auto& m : s_feed.due(now)) itsSend(s_lx, m.data(), m.size(), pdMS_TO_TICKS(200));
        if (s_feed.active() && s_lx < 0) {  // lxst let go of the session
            s_feed = kq::radio::Feed();
            Lock g;
            s_st.playing.clear();
        }
    }
}

}  // namespace

// On the kq task, as the engine hands a burst out: copied into PSRAM, the vector dropped.
void kqRadioHear(kq1::Clip&& c) {
    Burst b;
    b.who = c.who;
    for (int i = 0; i < 4 && i < (int)c.sender.size(); i++) b.speaker |= (uint32_t)c.sender[i] << (8 * i);
    b.n = c.bytes.size();
    b.at = nowMs();
    b.data = (uint8_t*)heap_caps_malloc(b.n ? b.n : 1, MALLOC_CAP_SPIRAM);
    Lock g;
    if (!b.data || b.n > kQueueBytes) {
        heap_caps_free(b.data);
        s_st.missed++;
        return;
    }
    memcpy(b.data, c.bytes.data(), b.n);
    while (s_qBytes + b.n > kQueueBytes && !s_q.empty()) {
        s_qBytes -= s_q.front().n;
        release(s_q.front());
        s_q.pop_front();
        s_st.missed++;
    }
    s_q.push_back(b);
    s_qBytes += b.n;
    s_st.queued = (unsigned)s_q.size();
    if (s_task) xTaskNotifyGive(s_task);
}

KqRadio kqRadio() {
    Lock g;
    return s_st;
}

void KqRadioService::onInit() {
    s_mu = xSemaphoreCreateMutex();
    // Below lxst (priority 2, core 1), which does the decoding.
    s_task = spawnTask(radioMain, "kq.radio", 8192, nullptr, 1, 1, STACK_PSRAM);
}
