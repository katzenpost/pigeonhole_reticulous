// SPDX-License-Identifier: AGPL-3.0-only
// The kq task: finds the nearest Pigeonhole replica that serves Channel mode, keeps one
// Channel to it, carries the replica client's frames over it, and runs the katzenqt1 client:
// the engine on this task, the executor on two workers whose box reads and writes come back
// here, since the Channel's ITS handle belongs to this task.
#include "kq_service.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <ctime>
#include <map>
#include <set>
#include <string>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "fs.h"
#include "pm.h"
#include "ports.h"
#include "rnsd.h"
#include "spangap.h"

#include <sodium.h>

#include "kq1_voucher.h"
#include "kq_bacap.h"
#include "kq_nike.h"
#include "kq_probe.h"
#include "kq_replica.h"
#include "kq_runtime.h"
#include "kq_store.h"
#include "kq_vault.h"
#include "kq_view.h"

static const char* TAG = "kq";
static constexpr char kAspect[] = "pigeonhole.replica";
static constexpr char kChanTag[] = "kq.rep";
static constexpr uint32_t kTickMs = 500, kReopenMs = 10000;
static constexpr size_t kAnnHeader = 1 + 16 + 16 + 64 + 32;
// CTIDH-1024 peaks near 300 KB of stack (variable-length arrays in the isogeny code), so the
// actions that run it get a worker of their own; box reads and writes keep a small one.
static constexpr uint32_t kHeavyStack = 384 * 1024, kLightStack = 32 * 1024;

static uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }
static int64_t nowS() { return (int64_t)time(nullptr); }

static std::string hexOf(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}

static TaskHandle_t s_task = nullptr;
static kq::ReplicaClient* s_client = nullptr;
static unsigned s_sent = 0, s_sendFail = 0, s_recv = 0;
static kq::Probe* s_probe = nullptr;

static void wake() { if (s_task) xTaskNotifyGive(s_task); }

// ---- replicas heard ----

struct Replica {
    uint8_t identity[16];
    uint8_t hops;
    uint8_t flags;  // bit 0 a bridge, bit 1 serves Channel mode
};
static std::map<std::string, Replica> s_replicas;  // dest hex -> what its announces said
static int s_annHandle = -1;

static void onAnnounce(int handle, size_t) {
    if (handle != s_annHandle) return;
    uint8_t buf[kAnnHeader + 64];
    size_t n = itsRecv(handle, buf, sizeof buf, 0);
    if (n < kAnnHeader) return;
    std::string dest = hexOf(buf + 1, 16);
    auto it = s_replicas.find(dest);
    if (it != s_replicas.end() && memcmp(it->second.identity, buf + 17, 16) != 0) return;  // the first identity holds
    Replica r;
    memcpy(r.identity, buf + 17, 16);
    r.hops = buf[0];
    r.flags = n > kAnnHeader ? buf[kAnnHeader] : 0;
    s_replicas[dest] = r;
}

static void onAnnounceDisc(int) { s_annHandle = -1; }

static void connectAnnounces() {
    if (s_annHandle >= 0) return;
    rnsd_announces_connect_t req = {};
    strncpy(req.aspect, kAspect, sizeof req.aspect - 1);
    s_annHandle = itsConnect("rnsd", RNSD_PORT_ANNOUNCES, &req, sizeof req, pdMS_TO_TICKS(2000), 0, onAnnounce,
                             onAnnounceDisc);
}

static const std::string* nearestChannelReplica() {
    const std::string* best = nullptr;
    int bestHops = 256;
    for (auto& [dest, r] : s_replicas)
        if ((r.flags & 2) && r.hops < bestHops) { best = &dest; bestHops = r.hops; }
    return best;
}

// ---- the Channel ----

static int s_chan = -1;
static std::string s_chanDest;
static bool s_chanUp = false;
static uint32_t s_chanClosedAt = 0;

static bool sendFrame(const kq::Bytes& frame) {
    if (s_chan < 0) return false;
    kq::Bytes m{(uint8_t)(kq::wire::kMsgType >> 8), (uint8_t)kq::wire::kMsgType};
    m.insert(m.end(), frame.begin(), frame.end());
    bool ok = itsSend(s_chan, m.data(), m.size(), pdMS_TO_TICKS(1000)) == m.size();
    ok ? s_sent++ : s_sendFail++;
    return ok;
}

static void onChanRecv(int handle, size_t) {
    if (handle != s_chan) return;
    static uint8_t buf[2048];
    for (;;) {
        size_t n = itsRecv(handle, buf, sizeof buf, 0);
        if (n == 0) return;
        if (n < 2 || ((uint16_t)(buf[0] << 8 | buf[1])) != kq::wire::kMsgType) continue;
        s_recv++;
        s_client->on_message(buf + 2, n - 2, nowMs());
    }
}

static void chanClosed() {
    if (s_chan >= 0) itsDisconnect(s_chan);
    s_chan = -1;
    s_chanUp = false;
    s_chanClosedAt = nowMs();
    s_client->reset();
}

static void onChanDisc(int) {
    s_chan = -1;
    chanClosed();
}

static void onChanState(const char* key, const char* val) {
    if (!val || !strstr(key, ".state")) return;
    if (!strcmp(val, "active") && !s_chanUp) {
        s_chanUp = true;
        info("kq: channel to replica %s up", s_chanDest.c_str());
        s_client->ready(nowMs());
    } else if (!strcmp(val, "failed") || !strcmp(val, "closed")) {
        warn("kq: channel to replica %s %s", s_chanDest.c_str(), val);
        chanClosed();
    }
}

static void openChannel() {
    if (s_chan >= 0 || nowMs() - s_chanClosedAt < kReopenMs) return;
    auto dest = nearestChannelReplica();
    if (!dest) return;
    uint8_t h[16];
    for (int i = 0; i < 16; i++) h[i] = (uint8_t)strtoul(dest->substr(2 * i, 2).c_str(), nullptr, 16);
    s_chanDest = *dest;
    s_chan = rnsdChannelOpen(h, kAspect, "", kChanTag, 0, 20000, 0, onChanRecv, onChanDisc);
    if (s_chan < 0) s_chanClosedAt = nowMs();
    else info("kq: opening channel to replica %s", dest->c_str());
}

// ---- the client: the runtime on this task, the executor on workers ----

static kq::Runtime* s_rt = nullptr;
static kq::Store* s_store = nullptr;  // once the chats are open
static kq::Vault* s_vault = nullptr;
static kq::ReplicaBackend* s_backend = nullptr;
static SemaphoreHandle_t s_mu = nullptr;  // the engine's state, which the CLI reads
static QueueHandle_t s_lightQ = nullptr, s_heavyQ = nullptr, s_doneQ = nullptr, s_bridgeQ = nullptr;
static bool s_lightIdle = true, s_heavyIdle = true;

static bool heavy(const kq1::Action& a) {
    return a.kind == kq1::Action::MINT || a.kind == kq1::Action::SEAL || a.kind == kq1::Action::AWAIT_REPLY;
}

struct Finished {
    kq1::Done done;
    bool heavy;
};

struct BridgeReq {
    std::function<void(std::function<void()>)> fn;
    SemaphoreHandle_t sem;
};

// On a worker: runs fn on the kq task, which owns the replica client, and waits for done.
static void bridge(std::function<void(std::function<void()>)> fn) {
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    auto* r = new BridgeReq{std::move(fn), sem};
    xQueueSend(s_bridgeQ, &r, portMAX_DELAY);
    wake();
    xSemaphoreTake(sem, portMAX_DELAY);
    vSemaphoreDelete(sem);
}

static void workerMain(void* arg) {
    bool isHeavy = arg != nullptr;
    QueueHandle_t q = isHeavy ? s_heavyQ : s_lightQ;
    kq1::Executor x{*s_backend, [](uint64_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); },
                    {[](const std::string& f) -> std::optional<kq::Bytes> {
                         auto d = s_vault->files().read(f);
                         return d ? std::optional<kq::Bytes>(kq::Bytes(d->begin(), d->end())) : std::nullopt;
                     },
                     [](const std::string& f) { s_vault->files().remove(f); }}};
    for (;;) {
        kq1::Action* a = nullptr;
        if (xQueueReceive(q, &a, portMAX_DELAY) != pdTRUE || !a) continue;
        // Tasks idle at the clock's floor; CTIDH is worth the full 240 MHz.
        if (isHeavy) pmBoost();
        auto* f = new Finished{x.perform(*a), isHeavy};
        if (isHeavy) pmBoostEnd();
        delete a;
        xQueueSend(s_doneQ, &f, portMAX_DELAY);
        wake();
    }
}

// On the kq task: what the workers asked of the replica, what they finished, what is due, what is next.
static void pumpClient() {
    BridgeReq* r;
    while (xQueueReceive(s_bridgeQ, &r, 0) == pdTRUE) {
        SemaphoreHandle_t sem = r->sem;
        r->fn([sem] { xSemaphoreGive(sem); });
        delete r;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    Finished* f;
    while (xQueueReceive(s_doneQ, &f, 0) == pdTRUE) {
        s_rt->finished(nowS(), nowMs(), f->done);
        (f->heavy ? s_heavyIdle : s_lightIdle) = true;
        delete f;
    }
    s_rt->tick(nowS(), nowMs());
    // The runtime hands out the next action of the kind an idle worker takes.
    while (auto a = s_rt->take_if([](const kq1::Action& x) {
        return heavy(x) ? s_heavyIdle : s_lightIdle;
    })) {
        bool h = heavy(*a);
        auto* p = new kq1::Action(std::move(*a));
        xQueueSend(h ? s_heavyQ : s_lightQ, &p, portMAX_DELAY);
        (h ? s_heavyIdle : s_lightIdle) = false;
    }
    xSemaphoreGive(s_mu);
}

static void probe(unsigned rounds) {
    if (!s_probe->start(rounds, [](const std::string& s) { info("kq probe: %s", s.c_str()); }))
        warn("kq probe: one is running, or no rounds asked");
}

// A file on this node's file system, sent to a group.
static std::optional<kq1::Input> sendFile(const std::string& group, const std::string& path) {
    int h = fs_open(path.c_str(), "rb");
    if (h < 0) { warn("kq sendfile: no file %s", path.c_str()); return std::nullopt; }
    kq::Bytes b;
    uint8_t buf[1024];
    size_t k;
    while ((k = fs_read(buf, 1, sizeof buf, h)) > 0) b.insert(b.end(), buf, buf + k);
    fs_close(h);
    kq1::Input in(kq1::Input::SEND_FILE);
    in.chat = group;
    in.name = path.substr(path.rfind('/') + 1);
    in.bytes = std::move(b);
    return in;
}

// Commands reach the kq task as storage keys "kq.cmd.<verb>" = "<group>|<argument>",
// since the Channel's ITS handle belongs to this task.
static void onCmd(const char* key, const char* val) {
    if (!val || !*val) return;
    std::string verb = strrchr(key, '.') + 1, v = val;
    storageUnset(key);
    if (verb == "probe") { probe((unsigned)atoi(val)); return; }
    if (!s_store) return;  // locked, or not chosen yet
    size_t bar = v.find('|');
    std::string group = v.substr(0, bar), arg = bar == std::string::npos ? "" : v.substr(bar + 1);
    auto in = kq::input_of(verb, group, arg);
    if (verb == "file") in = sendFile(group, arg);
    if (!in) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_rt->input(nowS(), nowMs(), *in);
    xSemaphoreGive(s_mu);
}

// s.kq.wire_version: the katzenqt1 wire version written and minted, 4 (compact vouchers) or 3
// (long ones), for founders that read only those: kpclientd on hpqc v0.0.95.
static void onWireVersion(const char*, const char*) {
    uint64_t v = storageGetInt("s.kq.wire_version", 4) == 3 ? kq1::MAP_VERSION : kq1::CURRENT_VERSION;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_rt->eng.wire_version = v;
    xSemaphoreGive(s_mu);
}

// ---- the chats on flash, under fsStateDir()/kq ----

static kq::StoreFiles fsFiles(const std::string& dir) {
    auto path = [dir](const std::string& n) { return dir + "/" + n; };
    auto put = [](const std::string& p, const std::string& d, const char* mode) {
        // Attachments and the spool are kept in directories of their own.
        size_t slash = p.rfind('/');
        if (slash != std::string::npos) fs_mkdirp(p.substr(0, slash).c_str());
        int f = fs_open(p.c_str(), mode);
        if (f < 0) return false;
        bool ok = fs_write(d.data(), 1, d.size(), f) == d.size() && fs_sync(f) == 0;
        fs_close(f);
        return ok;
    };
    kq::StoreFiles f;
    f.read = [path](const std::string& n) -> std::optional<std::string> {
        int h = fs_open(path(n).c_str(), "rb");
        if (h < 0) return std::nullopt;
        std::string s;
        char buf[1024];
        size_t k;
        while ((k = fs_read(buf, 1, sizeof buf, h)) > 0) s.append(buf, k);
        fs_close(h);
        return s;
    };
    f.replace = [path, put](const std::string& n, const std::string& d) {
        std::string tmp = path(n) + ".new";
        if (!put(tmp, d, "wb")) { fs_remove(tmp.c_str()); return false; }
        return fs_rename(tmp.c_str(), path(n).c_str()) == 0;
    };
    f.append = [path, put](const std::string& n, const std::string& d) { return put(path(n), d, "ab"); };
    f.remove = [path](const std::string& n) { fs_remove(path(n).c_str()); };
    return f;
}

// On the kq task, once the vault opens: what was saved becomes the runtime's state, and every
// step's changes are saved before its actions are handed out.
static void loadChats() {
    if (s_store) return;
    s_store = new kq::Store(s_vault->files());
    std::vector<std::string> problems;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_rt->eng.state = s_store->load(storageGetStr("s.kq.name", "kq"), &problems);
    s_rt->on_clip = [](kq1::Clip&& c) { kqRadioHear(std::move(c)); };
    s_rt->on_file = [](const kq1::FileWrite& f) {
        if (!s_vault->files().replace(f.file, std::string(f.bytes.begin(), f.bytes.end())))
            err("kq: keeping %s failed", f.file.c_str());
    };
    s_rt->on_changes = [](const kq1::Changes& ch) {
        if (!s_store->commit(s_rt->eng.state, ch)) err("kq: saving the chats failed");
    };
    s_rt->resume(nowS());
    size_t n = s_rt->eng.state.chats.size();
    xSemaphoreGive(s_mu);
    for (auto& p : problems) warn("kq: not loaded: %s", p.c_str());
    info("kq: %u chats loaded", (unsigned)n);
}

// ---- CLI ----

uint32_t kqGeneration() { return s_rt ? s_rt->steps.load() : 0; }

std::vector<KqChat> kqChats(const std::string& only, bool lines) {
    std::vector<KqChat> out;
    if (!s_mu || !s_rt) return out;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    out = kq::chats(s_rt->eng.state, only, lines);
    xSemaphoreGive(s_mu);
    return out;
}

// Open only once the chats are loaded, so that a screen never shows them empty in between.
KqLock kqLock(std::string* why) {
    KqLock l = s_vault ? s_vault->state(why) : KqLock::STARTING;
    return l == KqLock::OPEN && !s_store ? KqLock::UNLOCKING : l;
}
void kqSetup(const std::string& passphrase) { if (s_vault) { s_vault->ask_setup(passphrase); wake(); } }
void kqUnlock(const std::string& passphrase) { if (s_vault) { s_vault->ask_unlock(passphrase); wake(); } }

// ---- the radio: the groups in s.kq.radio, comma-separated; playing is the voice slice's ----

// Without the voice slice (conditional/lxst/) bursts are dropped unheard.
__attribute__((weak)) void kqRadioHear(kq1::Clip&&) {}
__attribute__((weak)) KqRadio kqRadio() { return {}; }

std::vector<std::string> kqRadioGroups() {
    std::vector<std::string> out;
    std::string all = storageGetStr("s.kq.radio", "");
    for (size_t p = 0, q; p < all.size(); p = q + 1) {
        q = all.find(',', p);
        if (q == std::string::npos) q = all.size();
        if (q > p) out.push_back(all.substr(p, q - p));
    }
    return out;
}

static void applyRadio() {
    auto gs = kqRadioGroups();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_rt->eng.radio = std::set<std::string>(gs.begin(), gs.end());
    xSemaphoreGive(s_mu);
}

void kqRadioSet(const std::string& group, bool on) {
    auto gs = kqRadioGroups();
    gs.erase(std::remove(gs.begin(), gs.end(), group), gs.end());
    if (on) gs.push_back(group);
    std::string all;
    for (auto& g : gs) all += (all.empty() ? "" : ",") + g;
    storageSet("s.kq.radio", all.c_str());
    applyRadio();
}

static void cliRadio(const std::string& how, const std::string& group) {
    if ((how == "on" || how == "off") && !group.empty()) kqRadioSet(group, how == "on");
    else if (!how.empty()) { cliPrintf("kq radio [on <g> | off <g>]\n"); return; }
    std::string gs;
    for (auto& g : kqRadioGroups()) gs += (gs.empty() ? "" : ", ") + g;
    KqRadio r = kqRadio();
    cliPrintf("radio groups: %s%s\n", gs.empty() ? "none" : gs.c_str(), r.voice ? "" : " (no voice in this image)");
    if (!r.voice) return;
    cliPrintf("playing: %s; queued %u, heard %u, missed %u\n", r.playing.empty() ? "-" : r.playing.c_str(), r.queued,
              r.heard, r.missed);
    if (!r.last.empty()) cliPrintf("last: %s\n", r.last.c_str());
}

void kqCommand(const char* verb, const std::string& group, const std::string& arg) {
    std::string v = arg.empty() ? group : group + "|" + arg;
    storageSet((std::string("kq.cmd.") + verb).c_str(), v.c_str());
}

static void printChats(const std::string& only, bool lines) {
    static const char* marks[] = {"", " \xE2\x80\xA6", " \xE2\x9C\x93", " \xE2\x9C\x93\xE2\x9C\x93"};
    for (auto& c : kqChats(only, lines)) {
        std::string st = c.joined ? "joined" : "not joined";
        if (!c.voucher.empty()) st += ", voucher " + c.voucher;
        if (c.adding) st += ", adding a member";
        if (c.pending) st += ", " + std::to_string(c.pending) + " to write";
        cliPrintf("%s: %s, %u members\n", c.id.c_str(), st.c_str(), c.members);
        for (auto& l : c.lines) {
            if (l.notice) cliPrintf("  \xE2\x80\x94 %s\n", l.text.c_str());
            else cliPrintf("  %s%s: %s\n", l.who.c_str(), marks[l.mark], l.text.c_str());
        }
    }
}

// ---- kq bench: CTIDH-1024 and its field timed on this core, to compare changes by ----
extern "C" {
void fiat_p1024_mul(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_square(uint64_t o[16], const uint64_t a[16]);
void fiat_p1024_add(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
void fiat_p1024_sub(uint64_t o[16], const uint64_t a[16], const uint64_t b[16]);
}
#if __XTENSA__
extern "C" {  // IDF's eri.h has no C++ guard
#include "eri.h"
}
#include "xtensa-debug-module.h"
#include "xtensa/xt_perf_consts.h"
// IDF's xtensa_perfmon_init/start/stop/value, from its xtensa component; counting at every
// interrupt level. The core has two counters, so a run counts two things.
static void perfStart(uint16_t sel0, uint16_t mask0, uint16_t sel1, uint16_t mask1) {
    uint16_t sel[2] = {sel0, sel1}, mask[2] = {mask0, mask1};
    eri_write(ERI_PERFMON_PGM, 0);
    for (int i = 0; i < 2; i++) {
        eri_write(ERI_PERFMON_PM0 + i * 4, 0);
        eri_write(ERI_PERFMON_PMSTAT0 + i * 4, PMSTAT_OVFL);  // sticky; a 1 clears it
        eri_write(ERI_PERFMON_PMCTRL0 + i * 4, (PMCTRL_TRACELEVEL_MASK << PMCTRL_TRACELEVEL_SHIFT) |
                                                   ((sel[i] & PMCTRL_SELECT_MASK) << PMCTRL_SELECT_SHIFT) |
                                                   ((uint32_t)mask[i] << PMCTRL_MASK_SHIFT));
    }
    eri_write(ERI_PERFMON_PGM, PGM_PMEN);
}
// The counts, or -1 for one that wrapped (32 bits: about 18 s of cycles at 240 MHz).
static void perfStop(int64_t out[2]) {
    eri_write(ERI_PERFMON_PGM, 0);
    for (int i = 0; i < 2; i++)
        out[i] = (eri_read(ERI_PERFMON_PMSTAT0 + i * 4) & PMSTAT_OVFL) ? -1 : eri_read(ERI_PERFMON_PM0 + i * 4);
}
#endif

static TaskHandle_t s_bench = nullptr;

// One field operation 1000 times, each result the next input: once counting cycles and
// instructions, once the cycles stalled on instruction- and data-cache misses.
static void benchField(const char* name, const std::function<void(uint64_t*)>& op) {
    static uint64_t x[16];
    randombytes_buf(x, sizeof x);
    x[15] &= (1ull << 59) - 1;  // below 2^1019, so below p
    int64_t c[4] = {-1, -1, -1, -1};
    int64_t t0 = esp_timer_get_time();
#if __XTENSA__
    perfStart(XTPERF_CNT_CYCLES, XTPERF_MASK_CYCLES, XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL);
#endif
    for (int i = 0; i < 1000; i++) op(x);
#if __XTENSA__
    perfStop(c);
#endif
    int64_t us = esp_timer_get_time() - t0;
#if __XTENSA__
    perfStart(XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_CACHE_MISS, XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_CACHE_MISS);
    for (int i = 0; i < 1000; i++) op(x);
    perfStop(c + 2);
#endif
    info("bench: field %-6s x1000 %8lld us | per op: cycles %lld, insns %lld, I-miss stall %lld, D-miss stall %lld",
         name, (long long)us, c[0] < 0 ? -1 : c[0] / 1000, c[1] < 0 ? -1 : c[1] / 1000,
         c[2] < 0 ? -1 : c[2] / 1000, c[3] < 0 ? -1 : c[3] / 1000);
}

// A whole CTIDH-1024 action takes minutes, past the counters' 32 bits: wall time only.
static void benchAction(const char* name, const std::function<void()>& f) {
    int64_t t0 = esp_timer_get_time();
    f();
    info("bench: %-14s %10.1f s", name, (esp_timer_get_time() - t0) / 1e6);
}

// n calls of f, wall time: the time per call.
static void benchOp(const char* name, int n, const std::function<void()>& f) {
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++) f();
    info("bench: %-28s x%-4d %9.1f us per call", name, n, (double)(esp_timer_get_time() - t0) / n);
}

// What every box written and read costs here, as test/bench's bacap entries on the host.
static void benchBacap() {
    namespace bacap = kq::bacap;
    static const uint8_t* ctx = (const uint8_t*)bacap::kPigeonholeCtx;
    static constexpr size_t n = sizeof bacap::kPigeonholeCtx - 1;
    auto rnd = [](uint8_t* p, size_t k) { randombytes_buf(p, k); };
    auto w = bacap::WriteCap::generate(rnd);
    auto r = w.read_cap();
    kq::Bytes text(200, 'a');
    auto padded = *kq::wire::pad(text.data(), text.size(), s_client->box_size());
    auto box = bacap::encrypt(w, w.index, ctx, n, padded.data(), padded.size());
    benchOp("box ID (blind the root key)", 100, [&] { bacap::box_id(r.root_pk, r.index, ctx, n); });
    benchOp("encrypt and sign a box", 100, [&] { bacap::encrypt(w, w.index, ctx, n, padded.data(), padded.size()); });
    benchOp("verify and decrypt a box", 100, [&] {
        bacap::decrypt(r.index, box.box_id.data(), ctx, n, box.ciphertext.data(), box.ciphertext.size(),
                       box.signature.data());
    });
    benchOp("next index", 100, [&] { w.index.next(); });
    benchOp("pad a box", 100, [&] { kq::wire::pad(text.data(), text.size(), s_client->box_size()); });
}

// A voucher join's CTIDH with a seal's two actions in turn, then at once: the inductee's induct (the
// payload's key checked, then the seal), and the minter's open without the seal public key at hand.
// One seed both ways, so the outputs must match bit for bit.
static void benchJoin() {
    namespace v = kq1::voucher;
    auto rnd = [](uint8_t* p, size_t n) { randombytes_buf(p, n); };
    kq::Bytes cap = kq::bacap::WriteCap::generate(rnd).bytes(), who(32, 7), salt(v::kSaltSize);
    randombytes_buf(salt.data(), salt.size());
    uint8_t seed[32];
    randombytes_buf(seed, sizeof seed);
    int64_t t0 = esp_timer_get_time();
    auto m = v::mint(cap, "bench", v::COMPACT, rnd);
    info("bench: join mint (one action) %.1f s", (esp_timer_get_time() - t0) / 1e6);
    if (!m) return;
    auto saved = kq::nike::both;  // a join running meanwhile takes this bench's mode: the same results
    kq::Bytes sealed[2], opened[2];
    for (int cores = 1; cores <= 2; cores++) {
        kq::nike::both = cores == 1 ? kq::nike::in_turn : kq::nike::at_once;
        kq::nike::Shake sh(seed, sizeof seed);
        t0 = esp_timer_get_time();
        auto r = v::induct(m->voucher.data(), m->payload, who, salt, sh.rng());
        int64_t t1 = esp_timer_get_time();
        auto o = r ? v::open_reply(m->secret_key, {}, r->sealed, cap) : std::nullopt;
        int64_t t2 = esp_timer_get_time();
        info("bench: join %d core%s: induct %.1f s, open %.1f s, both %.1f s", cores, cores == 1 ? " " : "s",
             (t1 - t0) / 1e6, (t2 - t1) / 1e6, (t2 - t0) / 1e6);
        if (r) sealed[cores - 1] = r->sealed;
        if (o) opened[cores - 1] = o->mutated_write_cap;
    }
    kq::nike::both = saved;
    bool same = !sealed[0].empty() && sealed[0] == sealed[1] && !opened[0].empty() && opened[0] == opened[1];
    info("bench: join outputs %s", same ? "the same, bit for bit" : "DIFFER");
}

enum BenchKind { BENCH_FIELD, BENCH_CTIDH, BENCH_BACAP, BENCH_JOIN };

static void benchMain(void* arg) {
    auto kind = (BenchKind)(intptr_t)arg;
    pmBoost();
    if (kind == BENCH_BACAP || kind == BENCH_JOIN) {
        kind == BENCH_BACAP ? benchBacap() : benchJoin();
        info("bench: done");
        pmBoostEnd();
        s_bench = nullptr;
        killSelf();
    }
    bool ctidh = kind == BENCH_CTIDH;
    info("bench: CTIDH field %s", KQ_CTIDH_FIELD_NAME);
    static uint64_t y[16];
    randombytes_buf(y, sizeof y);
    y[15] &= (1ull << 59) - 1;
    benchField("mul", [](uint64_t* x) { fiat_p1024_mul(x, x, y); });
    benchField("square", [](uint64_t* x) { fiat_p1024_square(x, x); });
    benchField("add", [](uint64_t* x) { fiat_p1024_add(x, x, y); });
    benchField("sub", [](uint64_t* x) { fiat_p1024_sub(x, x, y); });
    if (ctidh) {
        auto rnd = [](uint8_t* p, size_t n) { randombytes_buf(p, n); };
        kq::nike::Keypair a, b;
        benchAction("keypair", [&] { a = kq::nike::generate(rnd); });
        b = kq::nike::generate(rnd);
        benchAction("shared secret", [&] { kq::nike::derive_secret(a.priv, b.pub); });
        benchAction("valid_public", [&] { kq::nike::valid_public(b.pub); });
    }
    info("bench: done");
    pmBoostEnd();
    s_bench = nullptr;
    killSelf();
}

// On the heavy worker's core, with its stack.
static void cliBench(const std::string& what) {
    // radio: what a box costs at the replica, timed as kq probe times it.
    if (what.compare(0, 5, "radio") == 0) {
        std::string n = what.size() > 6 ? what.substr(6) : "10";
        storageSet("kq.cmd.probe", n.c_str());
        cliPrintf("kq bench radio: %s rounds queued; `kq` and `log` show the result\n", n.c_str());
        return;
    }
    if (what != "field" && what != "ctidh" && what != "bacap" && what != "join") {
        cliPrintf("kq bench field | ctidh | bacap | join | radio [rounds]\n");
        return;
    }
    if (s_bench) { cliPrintf("kq bench: one is running; `log` shows it\n"); return; }
    BenchKind k = what == "ctidh" ? BENCH_CTIDH : what == "bacap" ? BENCH_BACAP : what == "join" ? BENCH_JOIN : BENCH_FIELD;
    s_bench = spawnTask(benchMain, "kq.bench", kHeavyStack, (void*)(intptr_t)k, 1, 1, STACK_PSRAM);
    cliPrintf(s_bench ? "kq bench %s: started; the results go to the log\n" : "kq bench %s: no memory for the task\n",
              what.c_str());
}

static void cliKq(const char* args) {
    std::string a = args ? args : "";
    if (a.compare(0, 6, "bench ") == 0 || a == "bench") { cliBench(a.size() > 6 ? a.substr(6) : ""); return; }
    if (!s_task) { cliPrintf("kq: not started (rnsd is not up yet)\n"); return; }
    size_t sp = a.find(' ');
    std::string verb = a.substr(0, sp), rest = sp == std::string::npos ? "" : a.substr(sp + 1);
    sp = rest.find(' ');
    std::string group = rest.substr(0, sp), arg = sp == std::string::npos ? "" : rest.substr(sp + 1);
    // The passphrase goes to the vault directly: kept as a storage key, a browser would see it.
    if (verb == "setup" && (group == "none" || (group == "passphrase" && !arg.empty()))) {
        kqSetup(group == "none" ? "" : arg);
        sodium_memzero(a.data(), a.size());
        sodium_memzero(arg.data(), arg.size());
        cliPrintf("asked; `kq` shows the result\n");
        return;
    }
    if (verb == "unlock" && !rest.empty()) {
        kqUnlock(rest);
        sodium_memzero(a.data(), a.size());
        sodium_memzero(rest.data(), rest.size());
        cliPrintf("asked; `kq` shows the result\n");
        return;
    }
    if (verb == "probe") {
        storageSet("kq.cmd.probe", group.empty() ? "1" : group.c_str());
        cliPrintf("probe queued; `kq` shows the result\n");
        return;
    }
    if ((verb == "create" || verb == "join" || verb == "scan" || verb == "leave") && !group.empty()) {
        kqCommand(verb.c_str(), group);
        return;
    }
    if ((verb == "invite" || verb == "send" || verb == "sendfile") && !group.empty() && !arg.empty()) {
        if (verb == "sendfile") {
            int h = fs_open(arg.c_str(), "rb");
            if (h < 0) { cliPrintf("kq sendfile: no file %s\n", arg.c_str()); return; }
            fs_close(h);
            cliPrintf("kq sendfile: %s queued\n", arg.c_str());
        }
        // kq.cmd.sendfile is never delivered: storage keys stay short.
        kqCommand(verb == "sendfile" ? "file" : verb.c_str(), group, arg);
        return;
    }
    if (verb == "radio") { cliRadio(group, arg); return; }
    if (verb == "groups") { printChats("", false); return; }
    if (verb == "log" && !group.empty()) { printChats(group, true); return; }
    if (!a.empty() && verb != "status") {
        cliPrintf("kq [status] | probe [rounds] | groups | log <g> | create <g> | join <g> | invite <g> <code> | send <g> <text>\n"
                  "   | sendfile <g> <path> | radio [on <g> | off <g>]\n"
                  "   | bench field | ctidh | bacap | join | radio [rounds] | setup none | setup passphrase <passphrase> | unlock <passphrase>\n");
        return;
    }
    static const char* locks[] = {"starting", "no choice made yet: kq setup none | kq setup passphrase <p>",
                                  "locked: kq unlock <p>", "unlocking", "open", "cannot be opened"};
    std::string why;
    KqLock l = kqLock(&why);
    cliPrintf("chats: %s%s%s\n", locks[(int)l], why.empty() ? "" : "; ", why.c_str());
    if (s_vault && s_vault->last_ms)
        cliPrintf("passphrase: Argon2id over %u KiB, %u passes, took %u ms\n", (unsigned)(s_vault->last_mem >> 10),
                  (unsigned)s_vault->last_ops, (unsigned)s_vault->last_ms);
    cliPrintf("PSRAM: %u KiB free, %u KiB in one piece\n", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
              (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >> 10));
    cliPrintf("internal RAM: %u KiB free, %u KiB at the least so far\n",
              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
              (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) >> 10));
    cliPrintf("replicas heard: %u\n", (unsigned)s_replicas.size());
    for (auto& [d, r] : s_replicas) cliPrintf("  %s hops %d flags %d\n", d.c_str(), r.hops, r.flags);
    cliPrintf("channel: %s %s\n", s_chanDest.empty() ? "-" : s_chanDest.c_str(),
              s_chan < 0 ? "closed" : s_chanUp ? "up" : "opening");
    cliPrintf("requests pending: %u, box size %u, wire version %u\n", (unsigned)s_client->pending(),
              (unsigned)s_client->box_size(), (unsigned)s_rt->eng.wire_version);
    cliPrintf("frames sent %u, refused %u, received %u\n", s_sent, s_sendFail, s_recv);
    cliPrintf("probe: %s\n", s_probe->write.ms.empty() && !s_probe->write.failed && !s_probe->running()
                                ? "-" : s_probe->status().c_str());
    if (s_store) cliPrintf("saved: %u writes, %u bytes\n", (unsigned)s_store->writes, (unsigned)s_store->bytes);
}

// ---- task ----

static volatile bool s_stop = false, s_parked = false;

static void kqTaskMain(void*) {
    if (!itsServerInit()) err("kq itsServerInit failed");
    itsClientInit(4);
    storageSubscribeChanges("rnsd.chan.kq.rep", onChanState);
    std::string dir = fsStatePath("/kq");
    fs_mkdirp(dir.c_str());
    kq::Vault::room = [] { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); };
    kq::Vault::boost = [](bool on) { on ? pmBoost() : pmBoostEnd(); };
    s_vault = new kq::Vault(fsFiles(dir));
    storageSubscribeChanges("kq.cmd.", onCmd);
    storageSubscribeChanges("s.kq.wire_version", onWireVersion);
    onWireVersion(nullptr, nullptr);
    spawnTask(workerMain, "kq.io", kLightStack, nullptr, 1, 1, STACK_PSRAM);
    spawnTask(workerMain, "kq.ctidh", kHeavyStack, (void*)1, 1, 1, STACK_PSRAM);
    for (;;) {
        connectAnnounces();
        while (!s_stop) {
            itsPoll(pdMS_TO_TICKS(kTickMs));
            if (s_annHandle < 0) connectAnnounces();
            openChannel();
            s_client->tick(nowMs());
            uint32_t derived = s_vault->last_ms;
            if (s_vault->serve()) loadChats();
            if (s_vault->last_ms != derived)
                info("kq: Argon2id over %u KiB, %u passes: %u ms", (unsigned)(s_vault->last_mem >> 10),
                     (unsigned)s_vault->last_ops, (unsigned)s_vault->last_ms);
            if (s_store) pumpClient();
        }
        if (s_chan >= 0) chanClosed();
        if (s_annHandle >= 0) { itsDisconnect(s_annHandle); s_annHandle = -1; }
        s_parked = true;
        while (s_stop) itsPoll(portMAX_DELAY);
        s_parked = false;
    }
}

static void kqStart() {
    s_stop = false;
    if (sodium_init() < 0) { err("kq: sodium_init failed"); return; }
    if (!s_task) s_task = spawnTask(kqTaskMain, TAG, 16384, nullptr, 1, 1, STACK_PSRAM);
    else xTaskNotifyGive(s_task);
}

static void kqStop() {
    if (!s_task || s_stop) return;
    s_stop = true;
    xTaskNotifyGive(s_task);
    for (int i = 0; i < 300 && !s_parked; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

void KqService::onInit() {
    storageBegin();
    storageDefault("s.kq.box_size", 1549);
    storageDefault("s.kq.name", "kq");
    storageDefault("s.kq.wire_version", 4);
    storageDefault("s.kq.cores", 2);
    storageEnd();
    // s.kq.cores 1 runs a seal's two CTIDH actions in turn on the worker's core, to compare with 2.
    kq::nike::both = [](const kq::nike::Fn& a, const kq::nike::Fn& b) {
        (storageGetInt("s.kq.cores", 2) == 1 ? kq::nike::in_turn : kq::nike::at_once)(a, b);
    };
    s_client = new kq::ReplicaClient((size_t)storageGetInt("s.kq.box_size", 1549), sendFrame);
    s_probe = new kq::Probe(*s_client, nowMs, [](uint8_t* p, size_t n) { randombytes_buf(p, n); });
    s_mu = xSemaphoreCreateMutex();
    s_lightQ = xQueueCreate(1, sizeof(kq1::Action*));
    s_heavyQ = xQueueCreate(1, sizeof(kq1::Action*));
    s_doneQ = xQueueCreate(2, sizeof(Finished*));
    s_bridgeQ = xQueueCreate(2, sizeof(BridgeReq*));
    s_backend = new kq::ReplicaBackend(*s_client, bridge);
    s_rt = new kq::Runtime(kq1::State{storageGetStr("s.kq.name", "kq"), {}}, s_client->box_size());
    // A file is held whole in PSRAM as it is sent or fetched.
    s_rt->eng.max_file = 1 << 20;
    applyRadio();
    cliRegisterCmd("kq", cliKq);
    rnsServiceRegister(TAG, kqStart, kqStop);
}
