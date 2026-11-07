// SPDX-License-Identifier: AGPL-3.0-only
// The kq.sound task: a voucher code played to the device beside this one, or heard from it,
// through the audio task's stream port, which mixes what a client sends into the speaker and sends
// every client the microphone, before any gate or codec. A connection is held only for a transfer.
#include "kq_sound.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include <sodium.h>

#include "audio.h"
#include "av_hdr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "spangap.h"

#include "kq1_engine.h"
#include "kq_ggwave.h"
#include "kq_view.h"

namespace {

constexpr int kRate = 16000, kBlock = 320;  // the audio engine's 20 ms block at 16 kHz
constexpr int kQuiet = 10;                  // blocks of silence first, while the speaker comes up
constexpr int kLead = 4;                    // blocks sent ahead of playout: the engine banks 120 ms
constexpr size_t kMsg = sizeof(av_hdr_t) + 1920 * 2;

SemaphoreHandle_t s_mu = nullptr;
struct Lock {
    Lock() { xSemaphoreTake(s_mu, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mu); }
};
KqSound s_st;
enum Cmd { NONE, SEND, LISTEN, STOP, PLAY } s_cmd = NONE;
const int16_t* s_cmdPcm = nullptr;  // PLAY
size_t s_cmdN = 0;
std::string s_cmdCode, s_last;  // the last transfer, for the CLI
uint32_t s_until = 0;
TaskHandle_t s_task = nullptr;

int s_h = -1;  // the connection to the audio task
uint8_t* s_msg = nullptr;
int16_t* s_clip = nullptr;
int s_sent = 0, s_blocks = 0;  // of s_clip
uint32_t s_t0 = 0;
bool s_playing = false;        // a short clip (PLAY), not a voucher: s_st is left alone
kqgg::Rx* s_rx = nullptr;
int s_peak = 0;
bool s_heard = false;

uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

void set(KqSound::State st, const std::string& code = "", const std::string& why = "") {
    Lock g;
    s_st.state = st;
    s_st.code = code;
    s_st.why = why;
}

void end() {
    if (s_h >= 0) itsDisconnect(s_h);
    s_h = -1;
    s_playing = false;
    if (s_rx) pmBoostEnd();
    heap_caps_free(s_clip);
    delete s_rx;
    s_clip = nullptr;
    s_rx = nullptr;
}

void onDisc(int) { s_h = -1; }

void onRecv(int h, size_t) {
    for (size_t n; (n = itsRecv(h, s_msg, kMsg, 0)) > 0;) {
        if (!s_rx || s_heard || n <= sizeof(av_hdr_t)) continue;
        const av_hdr_t* hd = (const av_hdr_t*)s_msg;
        if (hd->type != AV_TYPE_AUDIO || hd->codec != AUDIO_PCM16) continue;
        const int16_t* pcm = (const int16_t*)(s_msg + sizeof(av_hdr_t));
        size_t ns = (n - sizeof(av_hdr_t)) / 2;
        for (size_t i = 0; i < ns; i++) s_peak = std::max(s_peak, std::abs((int)pcm[i]));
        uint8_t v[kqgg::kPayload];
        int64_t t = esp_timer_get_time();
        if (s_rx->push(pcm, ns, v, sizeof v) != kqgg::kPayload) continue;
        s_heard = true;
        std::string code = kq1::voucher_code(kq1::Bytes(v, v + sizeof v));
        char line[200];
        snprintf(line, sizeof line,
                 "heard a voucher, check %s, %.1f s into listening: decoded in %d ms, %d heard before it undecoded, "
                 "mic peak %.0f dBFS",
                 kq1::voucher_check(code).c_str(), (nowMs() - (s_until - KqSound::kListenMs)) / 1000.0,
                 (int)((esp_timer_get_time() - t) / 1000), s_rx->failed, 20 * log10(std::max(s_peak, 1) / 32768.0));
        info("kq sound: %s", line);
        sodium_memzero(v, sizeof v);
        set(KqSound::HEARD, code);
        Lock g;
        s_last = line;
    }
}

bool connect(const char* how) {
    s_h = itsConnect("audio", AUDIO_STREAM_PORT, how, strlen(how), pdMS_TO_TICKS(500), 0, onRecv, onDisc);
    return s_h >= 0;
}

void fail(const std::string& why) {
    end();
    warn("kq sound: %s", why.c_str());
    set(KqSound::FAILED, "", why);
    Lock g;
    s_last = why;
}

int volume() { return std::clamp(storageGetInt("s.kq.sound_volume", kqgg::kVolume), 1, 100); }

void startSend(const std::string& code) {
    auto v = kq1::from_code(code);
    if (!v) { fail("that is not a voucher code"); return; }
    set(KqSound::SENDING);  // the encoding takes a moment
    // The level: s.kq.sound_volume, ggwave's 0-100, 25 unless set: -25 dBFS RMS, for a device 10-30 cm away.
    auto pcm = kqgg::encode(v->data(), v->size(), volume());
    sodium_memzero(v->data(), v->size());
    s_blocks = kQuiet + (int)((pcm.size() + kBlock - 1) / kBlock) + 5;
    s_clip = pcm.empty() ? nullptr : (int16_t*)heap_caps_calloc(s_blocks * kBlock, 2, MALLOC_CAP_SPIRAM);
    if (!s_clip) { fail("no memory for the sound"); return; }
    memcpy(s_clip + kQuiet * kBlock, pcm.data(), pcm.size() * 2);
    if (!connect("pcm16,playback")) { fail("the speaker is in use"); return; }
    s_sent = 0;
    s_t0 = nowMs();
}

// After the same quiet lead a voucher has, while the speaker comes up.
void startPlay(const int16_t* pcm, size_t n) {
    end();
    s_blocks = kQuiet + (int)((n + kBlock - 1) / kBlock) + 1;
    s_clip = (int16_t*)heap_caps_calloc(s_blocks * kBlock, 2, MALLOC_CAP_SPIRAM);
    if (!s_clip) return;
    memcpy(s_clip + kQuiet * kBlock, pcm, n * 2);
    if (!connect("pcm16,playback")) {
        end();
        warn("kq sound: the speaker is in use; a ping is not played");
        return;
    }
    s_sent = 0;
    s_t0 = nowMs();
    s_playing = true;
}

void startListen() {
    s_rx = new (std::nothrow) kqgg::Rx;
    if (!s_rx || !s_rx->ok()) {
        delete s_rx;
        s_rx = nullptr;
        fail("no memory to listen");
        return;
    }
    pmBoost();  // the resampling and an FFT every 21 ms of sound, and the decode
    s_peak = 0;
    s_heard = false;
    if (!connect("pcm16")) { fail("the microphone is not there"); return; }
    {
        Lock g;
        s_until = nowMs() + KqSound::kListenMs;
    }
    set(KqSound::LISTENING);
}

void soundMain(void*) {
    itsClientInit(1);
    s_msg = (uint8_t*)heap_caps_malloc(kMsg, MALLOC_CAP_SPIRAM);
    for (;;) {
        if (s_h < 0) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        else itsPoll(pdMS_TO_TICKS(10));
        Cmd c;
        std::string code;
        KqSound::State st;
        const int16_t* pcm;
        size_t pcmN;
        {
            Lock g;
            c = s_cmd;
            s_cmd = NONE;
            pcm = s_cmdPcm;
            pcmN = s_cmdN;
            code = s_cmdCode;
            sodium_memzero(s_cmdCode.data(), s_cmdCode.size());
            s_cmdCode.clear();
            st = s_st.state;
        }
        // A clip waits for no voucher: it is dropped while one goes by sound.
        if (c == PLAY) {
            if (st != KqSound::SENDING && st != KqSound::LISTENING && storageGetInt("s.audio.rate", kRate) == kRate)
                startPlay(pcm, pcmN);
            continue;
        }
        if (c != NONE) {
            end();
            set(KqSound::IDLE);
            st = KqSound::IDLE;
            int rate = storageGetInt("s.audio.rate", kRate);
            if (c != STOP && rate != kRate) fail("the audio runs at " + std::to_string(rate) + " Hz, not 16000");
            else if (c == SEND) startSend(code);
            else if (c == LISTEN) startListen();
            sodium_memzero(code.data(), code.size());
            continue;
        }
        uint32_t now = nowMs();
        if (s_heard && s_rx) { end(); continue; }
        if (s_playing || st == KqSound::SENDING) {
            if (s_h < 0) {
                if (s_playing) end();
                else fail("the speaker went away");
                continue;
            }
            int due = std::min(s_blocks, kLead + (int)(now - s_t0) / 20);
            for (; s_sent < due; s_sent++) {
                avHdrFill((av_hdr_t*)s_msg, AV_TYPE_AUDIO, AUDIO_PCM16, 0, 0);
                memcpy(s_msg + sizeof(av_hdr_t), s_clip + s_sent * kBlock, kBlock * 2);
                itsSend(s_h, s_msg, sizeof(av_hdr_t) + kBlock * 2, pdMS_TO_TICKS(20));
            }
            if (s_sent == s_blocks && now - s_t0 > (uint32_t)s_blocks * 20 + 300) {
                bool clip = s_playing;
                end();
                if (clip) continue;
                set(KqSound::IDLE);
                info("kq sound: played a voucher, volume %d", volume());
                Lock g;
                s_last = "played a voucher";
            }
        } else if (st == KqSound::LISTENING) {
            if (s_h < 0) fail("the microphone went away");
            else if ((int32_t)(now - s_until) >= 0) {
                char why[120];
                snprintf(why, sizeof why, "heard no voucher in %u s (mic peak %.0f dBFS)",
                         (unsigned)(KqSound::kListenMs / 1000), 20 * log10(std::max(s_peak, 1) / 32768.0));
                fail(why);
            }
        }
    }
}

void command(Cmd c, const std::string& code = "") {
    if (!s_mu) return;
    {
        Lock g;
        s_cmd = c;
        s_cmdCode = code;
    }
    if (s_task) xTaskNotifyGive(s_task);
}

void cliSound(const char* args) {
    std::string a = args ? args : "";
    size_t sp = a.find(' ');
    std::string verb = a.substr(0, sp), group = sp == std::string::npos ? "" : a.substr(sp + 1);
    if (verb == "send" && !group.empty()) {
        auto c = kqChats(group);
        if (c.empty() || c[0].voucher.empty() || c[0].voucher == "\xE2\x80\xA6") {
            cliPrintf("kqsound: %s has no voucher to give out\n", group.c_str());
            return;
        }
        kqSoundSend(c[0].voucher);
        cliPrintf("kqsound: playing %s's voucher, check %s\n", group.c_str(), kq1::voucher_check(c[0].voucher).c_str());
        return;
    }
    if (verb == "listen") { kqSoundListen(); cliPrintf("kqsound: listening for %u s\n", (unsigned)(KqSound::kListenMs / 1000)); return; }
    if (verb == "stop") { kqSoundStop(); return; }
    if (!a.empty()) {
        cliPrintf("kqsound [send <g> | listen | stop]: a voucher by sound; s.kq.sound_volume sets the level (25)\n");
        return;
    }
    KqSound s = kqSound();
    static const char* names[] = {"idle", "sending", "listening", "heard", "failed"};
    cliPrintf("state %s%s%s\n", names[s.state], s.state == KqSound::HEARD ? ": " : "", s.code.c_str());
    Lock g;
    if (!s_last.empty()) cliPrintf("last  %s\n", s_last.c_str());
}

}  // namespace

KqSound kqSound() {
    if (!s_mu) return {};
    Lock g;
    KqSound s = s_st;
    if (s.state == KqSound::LISTENING) {
        int32_t left = (int32_t)(s_until - nowMs());
        s.left_ms = left > 0 ? (uint32_t)left : 0;
    }
    return s;
}

bool kqSoundBusy() {
    if (!s_mu) return false;
    Lock g;
    return s_st.state == KqSound::SENDING || s_st.state == KqSound::LISTENING;
}

void kqSoundSend(const std::string& code) { command(SEND, code); }
void kqSoundListen() { command(LISTEN); }
void kqSoundStop() { command(STOP); }

void kqSoundPlay(const int16_t* pcm, size_t n) {
    if (!s_mu) return;
    {
        Lock g;
        if (s_cmd != NONE) return;  // a voucher command first
        s_cmd = PLAY;
        s_cmdPcm = pcm;
        s_cmdN = n;
    }
    if (s_task) xTaskNotifyGive(s_task);
}

void KqSoundService::onInit() {
    s_mu = xSemaphoreCreateMutex();
    s_task = spawnTask(soundMain, "kq.sound", 12288, nullptr, 1, CORE_PRIMARY, STACK_PSRAM);
    cliRegisterCmd("kqsound", cliSound);
}
