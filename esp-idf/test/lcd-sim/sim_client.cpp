// SPDX-License-Identifier: AGPL-3.0-only
// kq_view.h for the simulator: the same runtime, replica client and executor the kq task runs,
// on threads, with the Channel carried by relay.py over a unix socket.
#include <sodium.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <future>
#include <mutex>
#include <set>
#include <algorithm>
#include <thread>

#include "kq_runtime.h"
#include "kq_store.h"
#include "kq_vault.h"
#include "kq_ogg.h"
#include "kq_ping_sounds.h"
#include "kq_radio_feed.h"
#include "kq_view.h"
#include "fskv.h"
#include "fskv_room.h"
#include "kq1_engine.h"
#include "sim.h"

void simHear(kq1::Clip&& c);
void simPinged(const kq1::Ping& p);

namespace {

std::mutex g_mu;  // the runtime and the replica client
kq::ReplicaClient* g_client;
kq::Runtime* g_rt;
kq::ReplicaBackend* g_backend;
kq::Store* g_store;
kq::Vault* g_vault;
int g_sock = -1;
bool g_up = false;
std::string g_path;

uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t nowS() { return (int64_t)time(nullptr); }

bool sendFrame(const kq::Bytes& f) {
    if (g_sock < 0 || !g_up) return false;
    uint8_t h[2] = {(uint8_t)(f.size() >> 8), (uint8_t)f.size()};
    return write(g_sock, h, 2) == 2 && write(g_sock, f.data(), f.size()) == (ssize_t)f.size();
}

void closeSock() {
    if (g_sock >= 0) close(g_sock);
    g_sock = -1;
    g_up = false;
    g_client->reset();
}

// Reads what has arrived; under g_mu.
void readSock() {
    static kq::Bytes buf;
    pollfd p{g_sock, POLLIN, 0};
    while (g_sock >= 0 && poll(&p, 1, 0) > 0) {
        uint8_t tmp[4096];
        ssize_t n = read(g_sock, tmp, sizeof tmp);
        if (n <= 0) { fprintf(stderr, "sim: relay closed\n"); closeSock(); buf.clear(); return; }
        buf.insert(buf.end(), tmp, tmp + n);
        while (buf.size() >= 2) {
            size_t len = (size_t)buf[0] << 8 | buf[1];
            if (buf.size() < 2 + len) break;
            if (len == 0) {
                g_up = true;
                fprintf(stderr, "sim: channel up\n");
                g_client->ready((uint32_t)nowMs());
            } else {
                g_client->on_message(buf.data() + 2, len, (uint32_t)nowMs());
            }
            buf.erase(buf.begin(), buf.begin() + 2 + len);
        }
    }
}

void connectSock() {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, g_path.c_str(), sizeof a.sun_path - 1);
    if (connect(s, (sockaddr*)&a, sizeof a) < 0) { close(s); return; }
    g_sock = s;
}

bool heavy(const kq1::Action& a) {
    return a.kind == kq1::Action::MINT || a.kind == kq1::Action::SEAL || a.kind == kq1::Action::AWAIT_REPLY;
}

struct Worker {
    std::mutex m;
    std::condition_variable cv;
    std::deque<kq1::Action> q;
    bool idle = true;  // under g_mu
};
Worker g_light, g_heavy;

void workerMain(Worker* w) {
    kq1::Executor x{*g_backend, [](uint64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }};
    for (;;) {
        std::unique_lock<std::mutex> l(w->m);
        w->cv.wait(l, [w] { return !w->q.empty(); });
        kq1::Action a = std::move(w->q.front());
        w->q.pop_front();
        l.unlock();
        kq1::Done d = x.perform(a);
        std::lock_guard<std::mutex> g(g_mu);
        g_rt->finished(nowS(), nowMs(), d);
        w->idle = true;
    }
}

// The executor's replica calls run under g_mu, where the kq loop also drives the client.
void bridge(std::function<void(std::function<void()>)> fn) {
    std::promise<void> p;
    {
        std::lock_guard<std::mutex> g(g_mu);
        fn([&p] { p.set_value(); });
    }
    p.get_future().wait();
}

std::string g_name, g_stateDir;

// Under g_mu, once the vault opens: as the kq task's loadChats.
void loadChats() {
    g_store = new kq::Store(g_vault->files());
    std::vector<std::string> problems;
    g_rt->eng.state = g_store->load(g_name, &problems);
    for (auto& p : problems) fprintf(stderr, "sim: not loaded: %s\n", p.c_str());
    fprintf(stderr, "sim: %zu chats loaded\n", g_rt->eng.state.chats.size());
    g_rt->on_clip = [](kq1::Clip&& c) { simHear(std::move(c)); };
    g_rt->on_ping = [](const kq1::Ping& p) { simPinged(p); };
    g_rt->on_changes = [](const kq1::Changes& ch) {
        if (!g_store->commit(g_rt->eng.state, ch)) fprintf(stderr, "sim: saving the chats failed\n");
    };
    g_rt->resume(nowS());
}

void kqLoop() {
    uint64_t lastTry = 0;
    for (;;) {
        bool opened = g_vault->serve();  // Argon2 takes a while: not under g_mu
        {
            std::lock_guard<std::mutex> g(g_mu);
            if (g_sock < 0 && nowMs() - lastTry > 2000) { lastTry = nowMs(); connectSock(); }
            if (g_sock >= 0) readSock();
            g_client->tick((uint32_t)nowMs());
            if (opened) loadChats();
            if (g_store) {
                g_rt->tick(nowS(), nowMs());
                while (auto a = g_rt->take_if([](const kq1::Action& x) { return (heavy(x) ? g_heavy : g_light).idle; })) {
                    Worker& w = heavy(*a) ? g_heavy : g_light;
                    w.idle = false;
                    std::lock_guard<std::mutex> l(w.m);
                    w.q.push_back(std::move(*a));
                    w.cv.notify_one();
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace

void simClientStart(const std::string& socketPath, const std::string& name, size_t boxSize, const std::string& stateDir) {
    if (sodium_init() < 0) { fprintf(stderr, "sim: sodium_init failed\n"); exit(1); }
    g_path = socketPath;
    // As the device: written at the mesh size, boxSize read too.
    g_client = new kq::ReplicaClient(kq::wire::kBoxMesh, sendFrame, {boxSize});
    g_backend = new kq::ReplicaBackend(*g_client, bridge);
    g_rt = new kq::Runtime(kq1::State{name, {}}, kq::wire::kBoxMesh);
    g_name = name;
    g_stateDir = stateDir;
    g_vault = new kq::Vault(kq::posix_files(stateDir));
    std::thread(workerMain, &g_light).detach();
    std::thread(workerMain, &g_heavy).detach();
    std::thread(kqLoop).detach();
}

KqLock kqLock(std::string* why) {
    KqLock l = g_vault ? g_vault->state(why) : KqLock::STARTING;
    std::lock_guard<std::mutex> g(g_mu);
    return l == KqLock::OPEN && !g_store ? KqLock::UNLOCKING : l;
}
void kqSetup(const std::string& passphrase) { if (g_vault) g_vault->ask_setup(passphrase); }
void kqUnlock(const std::string& passphrase) { if (g_vault) g_vault->ask_unlock(passphrase); }

std::string simSaved() {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_store) return "nothing: locked";
    return std::to_string(g_store->writes) + " writes, " + std::to_string(g_store->bytes) + " bytes";
}

bool simChannelUp() {
    std::lock_guard<std::mutex> g(g_mu);
    return g_up;
}

uint32_t kqGeneration() { return g_rt ? g_rt->steps.load() : 0; }

std::vector<KqChat> kqChats(const std::string& only, bool lines) {
    std::lock_guard<std::mutex> g(g_mu);
    return kq::chats(g_rt->eng.state, only, lines);
}

// The radio: the sim has no audio, so a burst is counted, its length read, and dropped.
namespace {
std::vector<std::string> g_radio;
KqRadio g_radioState;
std::string g_ping;  // shown 10 s from g_pingAt
uint64_t g_pingAt = 0;
}

void simHear(kq1::Clip&& c) {
    auto o = kq::ogg::demux(c.bytes.data(), c.bytes.size());
    uint64_t us = 0;
    if (o) for (auto& p : o->packets) us += kq::ogg::packet_us(p);
    char line[160];
    snprintf(line, sizeof line, "%s, %.1f s: fetched; no audio in the sim", c.who.c_str(), us / 1e6);
    g_radioState.heard++;
    g_radioState.last = line;
    fprintf(stderr, "sim: radio: %s\n", line);
}

std::vector<std::string> kqRadioGroups() {
    std::lock_guard<std::mutex> g(g_mu);
    return g_radio;
}

void kqRadioSet(const std::string& group, bool on) {
    std::lock_guard<std::mutex> g(g_mu);
    g_radio.erase(std::remove(g_radio.begin(), g_radio.end(), group), g_radio.end());
    if (on) g_radio.push_back(group);
    g_rt->eng.radio = std::set<std::string>(g_radio.begin(), g_radio.end());
}

std::string g_talk;
kq::radio::TalkClock g_clock;
std::string kqRadioTalk() {
    std::lock_guard<std::mutex> g(g_mu);
    return g_talk;
}
void kqRadioSetTalk(const std::string& group) {
    if (!group.empty()) kqRadioSet(group, true);
    std::lock_guard<std::mutex> g(g_mu);
    g_talk = group;
}

KqRadio kqRadio() {
    std::lock_guard<std::mutex> g(g_mu);
    KqRadio r = g_radioState;
    r.talking = g_clock.on;
    r.talk_left_ms = g_clock.left_ms((uint32_t)nowMs());
    r.talk_max_ms = kq::radio::kTalkMaxMs;
    if (!g_ping.empty() && nowMs() - g_pingAt < 10000) r.ping = g_ping;
    return r;
}

// Pings as the device makes them; a sound played goes to <state>/<name>.wav.
namespace {
kq::radio::PingGate g_pingOut, g_pingIn;
void play(const char* name, const int16_t* pcm, size_t n) {
    std::string path = g_stateDir + "/" + name + ".wav";
    wav_write(path.c_str(), pcm, n);
    fprintf(stderr, "sim: sound: played %s (%zu samples) into %s\n", name, n, path.c_str());
}
}  // namespace

bool kqRadioPing() {
    std::string talk = kqRadioTalk();
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (talk.empty() || !g_pingOut.take((uint32_t)nowMs())) return false;
        g_ping = "Pinged " + talk;
        g_pingAt = nowMs();
    }
    play("ping-sent", kPingSent, std::size(kPingSent));
    kqCommand("ping", talk);
    return true;
}

// Under g_mu, from the runtime.
void simPinged(const kq1::Ping& p) {
    time_t t = time(nullptr);
    char at[8] = "";
    struct tm tm;
    if (t >= 1704067200 && localtime_r(&t, &tm)) strftime(at, sizeof at, "%H:%M", &tm);  // before 2024: unset
    g_ping = p.who + " pinged" + (*at ? std::string(" at ") + at : "");
    g_pingAt = nowMs();
    fprintf(stderr, "sim: radio: %s in %s\n", g_ping.c_str(), p.chat.c_str());
    if (g_pingIn.take((uint32_t)nowMs())) play("ping-heard", kPingHeard, std::size(kPingHeard));
}

void simTalk(bool down) {
    std::lock_guard<std::mutex> g(g_mu);
    if (down) g_clock.start((uint32_t)nowMs());
    else g_clock.stop();
}

void kqCommand(const char* verb, const std::string& group, const std::string& arg) {
    auto in = kq::input_of(verb, group, arg);
    if (!in) return;
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_store) return;
    g_rt->input(nowS(), nowMs(), *in);
}

// The sound: Send by sound writes what the speaker would play to <state>/sound.wav, and simListen
// plays a recording to the microphone 20 ms at a time, through the device's receiver.
namespace {
KqSound g_sound;
uint64_t g_soundUntil = 0;
}

KqSound kqSound() {
    std::lock_guard<std::mutex> g(g_mu);
    uint64_t now = nowMs();
    if (g_sound.state == KqSound::SENDING && now >= g_soundUntil) g_sound.state = KqSound::IDLE;
    if (g_sound.state == KqSound::LISTENING && now >= g_soundUntil) {
        g_sound.state = KqSound::FAILED;
        g_sound.why = "heard no voucher in 15 s";
    }
    KqSound s = g_sound;
    if (s.state == KqSound::LISTENING) s.left_ms = (uint32_t)(g_soundUntil - now);
    return s;
}

bool kqSoundBusy() {
    KqSound s = kqSound();
    return s.state == KqSound::SENDING || s.state == KqSound::LISTENING;
}

void kqSoundSend(const std::string& code) {
    auto v = kq1::from_code(code);
    if (!v) return;
    std::vector<int16_t> pcm(16000 / 5 + FSKV_SAMPLES + 16000 / 10);
    fskv_modulate(v->data(), 9268, pcm.data() + 16000 / 5);  // -20 dBFS, as the device plays it
    std::string path = g_stateDir + "/sound.wav";
    wav_write(path.c_str(), pcm.data(), pcm.size());
    fprintf(stderr, "sim: sound: played %s's voucher into %s\n", code.c_str(), path.c_str());
    std::lock_guard<std::mutex> g(g_mu);
    g_sound = KqSound{};
    g_sound.state = KqSound::SENDING;
    g_soundUntil = nowMs() + pcm.size() / 16 + 300;
}

void kqSoundListen() {
    std::lock_guard<std::mutex> g(g_mu);
    g_sound = KqSound{};
    g_sound.state = KqSound::LISTENING;
    g_soundUntil = nowMs() + KqSound::kListenMs;
}

void kqSoundStop() {
    std::lock_guard<std::mutex> g(g_mu);
    g_sound = KqSound{};
}

void simListen(const std::string& wav) {
    std::vector<int16_t> x(16000 * 15);
    x.resize(wav_read(wav.c_str(), x.data(), x.size()));
    static int16_t buf[FSKV_RX_MIN];
    static fskv_rx rx;
    fskv_rx_init(&rx, buf, FSKV_RX_MIN);
    uint8_t v[32];
    for (size_t at = 0; at < x.size(); at += 320) {
        if (kqSound().state != KqSound::LISTENING) return;
        if (!fskv_rx_push(&rx, x.data() + at, std::min<size_t>(320, x.size() - at), v)) continue;
        std::string code = kq1::voucher_code(kq1::Bytes(v, v + 32));
        fprintf(stderr, "sim: sound: heard %s, RS fixed %d, clock %.4f\n", code.c_str(), rx.st.rs_fixed, rx.st.a);
        std::lock_guard<std::mutex> g(g_mu);
        g_sound.state = KqSound::HEARD;
        g_sound.code = code;
        return;
    }
    fprintf(stderr, "sim: sound: no voucher in %s (%zu samples)\n", wav.c_str(), x.size());
}
