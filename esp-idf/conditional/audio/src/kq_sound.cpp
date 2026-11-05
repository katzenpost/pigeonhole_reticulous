// SPDX-License-Identifier: AGPL-3.0-only
// The kq.sound task: a voucher code played to the device beside this one, or heard from it,
// through the audio task's stream port, which mixes what a client sends into the speaker and sends
// every client the microphone, before any gate or codec. A connection is held only for a transfer.
#include "kq_sound.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <sodium.h>

#include "audio.h"
#include "av_hdr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "spangap.h"

#include "fskv.h"
#include "kq1_engine.h"
#include "kq_view.h"

namespace {

constexpr int kRate = 16000, kBlock = 320;  // the audio engine's 20 ms block at 16 kHz
constexpr int kQuiet = 10;                  // blocks of silence first, while the speaker comes up
constexpr int kBlocks = kQuiet + FSKV_SAMPLES / kBlock + 5;
constexpr int kLead = 4;                    // blocks sent ahead of playout: the engine banks 120 ms
constexpr size_t kMsg = sizeof(av_hdr_t) + 1920 * 2;

SemaphoreHandle_t s_mu = nullptr;
struct Lock {
    Lock() { xSemaphoreTake(s_mu, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mu); }
};
KqSound s_st;
enum Cmd { NONE, SEND, LISTEN, STOP } s_cmd = NONE;
std::string s_cmdCode, s_last;  // the last transfer, for the CLI
uint32_t s_until = 0;
TaskHandle_t s_task = nullptr;

int s_h = -1;  // the connection to the audio task
uint8_t* s_msg = nullptr;
int16_t* s_clip = nullptr;
int s_sent = 0;
uint32_t s_t0 = 0;
fskv_rx* s_rx = nullptr;
int16_t* s_rxBuf = nullptr;
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
    if (s_rx) pmBoostEnd();
    heap_caps_free(s_clip);
    heap_caps_free(s_rx);
    heap_caps_free(s_rxBuf);
    s_clip = s_rxBuf = nullptr;
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
        uint8_t v[FSKV_PAYLOAD];
        int64_t t = esp_timer_get_time();
        if (!fskv_rx_push(s_rx, pcm, ns, v)) continue;
        s_heard = true;
        std::string code = kq1::voucher_code(kq1::Bytes(v, v + sizeof v));
        char line[200];
        snprintf(line, sizeof line,
                 "heard a voucher, check %s, %.1f s into listening: RS fixed %d, clock %.4f, %d tries, decoded in %d ms, "
                 "mic peak %.0f dBFS",
                 kq1::voucher_check(code).c_str(), (nowMs() - (s_until - KqSound::kListenMs)) / 1000.0, s_rx->st.rs_fixed,
                 s_rx->st.a, s_rx->st.tries, (int)((esp_timer_get_time() - t) / 1000),
                 20 * log10(std::max(s_peak, 1) / 32768.0));
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

void startSend(const std::string& code) {
    auto v = kq1::from_code(code);
    s_clip = (int16_t*)heap_caps_calloc(kBlocks * kBlock, 2, MALLOC_CAP_SPIRAM);
    if (!v || !s_clip) { fail(!v ? "that is not a voucher code" : "no memory for the sound"); return; }
    // The level: s.kq.sound_dbfs, RMS, -20 unless set; quiet, for a device 10-30 cm away.
    int dbfs = std::min(storageGetInt("s.kq.sound_dbfs", -20), -9);
    fskv_modulate(v->data(), (float)(32768 * pow(10, dbfs / 20.0) * 2 * M_SQRT2), s_clip + kQuiet * kBlock);
    sodium_memzero(v->data(), v->size());
    if (!connect("pcm16,playback")) { fail("the speaker is in use"); return; }
    s_sent = 0;
    s_t0 = nowMs();
    set(KqSound::SENDING);
}

void startListen() {
    s_rx = (fskv_rx*)heap_caps_malloc(sizeof *s_rx, MALLOC_CAP_SPIRAM);
    s_rxBuf = (int16_t*)heap_caps_malloc(FSKV_RX_MIN * 2, MALLOC_CAP_SPIRAM);
    if (!s_rx || !s_rxBuf) {
        heap_caps_free(s_rx);
        s_rx = nullptr;
        fail("no memory to listen");
        return;
    }
    pmBoost();  // an FFT every 4 ms of sound, and the decode
    fskv_rx_init(s_rx, s_rxBuf, FSKV_RX_MIN);
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
        {
            Lock g;
            c = s_cmd;
            s_cmd = NONE;
            code = s_cmdCode;
            sodium_memzero(s_cmdCode.data(), s_cmdCode.size());
            s_cmdCode.clear();
            st = s_st.state;
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
        if (st == KqSound::SENDING) {
            if (s_h < 0) { fail("the speaker went away"); continue; }
            int due = std::min(kBlocks, kLead + (int)(now - s_t0) / 20);
            for (; s_sent < due; s_sent++) {
                avHdrFill((av_hdr_t*)s_msg, AV_TYPE_AUDIO, AUDIO_PCM16, 0, 0);
                memcpy(s_msg + sizeof(av_hdr_t), s_clip + s_sent * kBlock, kBlock * 2);
                itsSend(s_h, s_msg, sizeof(av_hdr_t) + kBlock * 2, pdMS_TO_TICKS(20));
            }
            if (s_sent == kBlocks && now - s_t0 > kBlocks * 20 + 300) {
                end();
                set(KqSound::IDLE);
                info("kq sound: played a voucher, %d dBFS", std::min(storageGetInt("s.kq.sound_dbfs", -20), -9));
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
        cliPrintf("kqsound [send <g> | listen | stop]: a voucher by sound; s.kq.sound_dbfs sets the level (-20)\n");
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

void KqSoundService::onInit() {
    s_mu = xSemaphoreCreateMutex();
    s_task = spawnTask(soundMain, "kq.sound", 12288, nullptr, 1, CORE_PRIMARY, STACK_PSRAM);
    cliRegisterCmd("kqsound", cliSound);
}
