// SPDX-License-Identifier: AGPL-3.0-only
// The kq task: finds the nearest Pigeonhole replica that serves Channel mode, keeps one
// Channel to it, carries the replica client's frames over it, and runs the katzenqt1 client:
// the engine on this task, the executor on two workers whose box reads and writes come back
// here, since the Channel's ITS handle belongs to this task.
#include "kq_service.h"

#include <cstring>
#include <ctime>
#include <map>
#include <string>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fs.h"
#include "ports.h"
#include "rnsd.h"
#include "spangap.h"

#include <sodium.h>

#include "kq_replica.h"
#include "kq_runtime.h"
#include "kq_store.h"
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
static std::string s_probe = "-";

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
    kq1::Executor x{*s_backend, [](uint64_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }};
    for (;;) {
        kq1::Action* a = nullptr;
        if (xQueueReceive(q, &a, portMAX_DELAY) != pdTRUE || !a) continue;
        auto* f = new Finished{x.perform(*a), isHeavy};
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

static void probe() {
    auto w = kq::bacap::WriteCap::generate([](uint8_t* p, size_t n) { randombytes_buf(p, n); });
    kq::Bytes text{'k', 'q', ' ', 'p', 'r', 'o', 'b', 'e'};
    auto r = w.read_cap();
    s_probe = "writing";
    bool ok = s_client->write(w, w.index, text, [r, text](uint8_t code) {
        s_probe = "write code " + std::to_string(code);
        if (code != kq::wire::SUCCESS) return;
        s_client->read(r.root_pk, r.index, [text](const kq::ReadOutcome& o) {
            bool same = o.kind == kq::ReadOutcome::FOUND && o.plaintext == text;
            s_probe = same ? "ok: written and read back" : "read kind " + std::to_string((int)o.kind);
        });
    });
    if (!ok) s_probe = "refused";
}

// Commands reach the kq task as storage keys "kq.cmd.<verb>" = "<group>|<argument>",
// since the Channel's ITS handle belongs to this task.
static void onCmd(const char* key, const char* val) {
    if (!val || !*val) return;
    std::string verb = strrchr(key, '.') + 1, v = val;
    storageUnset(key);
    if (verb == "probe") { probe(); return; }
    size_t bar = v.find('|');
    std::string group = v.substr(0, bar), arg = bar == std::string::npos ? "" : v.substr(bar + 1);
    auto in = kq::input_of(verb, group, arg);
    if (!in) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_rt->input(nowS(), nowMs(), *in);
    xSemaphoreGive(s_mu);
}

// ---- the chats on flash, under fsStateDir()/kq ----

static kq::Store* s_store = nullptr;

static kq::StoreFiles fsFiles(const std::string& dir) {
    auto path = [dir](const std::string& n) { return dir + "/" + n; };
    auto put = [](const std::string& p, const std::string& d, const char* mode) {
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

// On the kq task, once: what was saved becomes the runtime's state, and every step's changes
// are saved before its actions are handed out.
static void loadChats() {
    if (s_store) return;
    std::string dir = fsStatePath("/kq");
    fs_mkdirp(dir.c_str());
    s_store = new kq::Store(fsFiles(dir));
    std::vector<std::string> problems;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_rt->eng.state = s_store->load(storageGetStr("s.kq.name", "kq"), &problems);
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

static void cliKq(const char* args) {
    std::string a = args ? args : "";
    if (!s_task) { cliPrintf("kq: not started (rnsd is not up yet)\n"); return; }
    size_t sp = a.find(' ');
    std::string verb = a.substr(0, sp), rest = sp == std::string::npos ? "" : a.substr(sp + 1);
    sp = rest.find(' ');
    std::string group = rest.substr(0, sp), arg = sp == std::string::npos ? "" : rest.substr(sp + 1);
    if (verb == "probe") { storageSet("kq.cmd.probe", "1"); cliPrintf("probe queued; `kq` shows the result\n"); return; }
    if ((verb == "create" || verb == "join" || verb == "scan" || verb == "leave") && !group.empty()) {
        kqCommand(verb.c_str(), group);
        return;
    }
    if ((verb == "invite" || verb == "send") && !group.empty() && !arg.empty()) {
        kqCommand(verb.c_str(), group, arg);
        return;
    }
    if (verb == "groups") { printChats("", false); return; }
    if (verb == "log" && !group.empty()) { printChats(group, true); return; }
    if (!a.empty() && verb != "status") {
        cliPrintf("kq [status] | probe | groups | log <g> | create <g> | join <g> | invite <g> <code> | send <g> <text>\n");
        return;
    }
    cliPrintf("replicas heard: %u\n", (unsigned)s_replicas.size());
    for (auto& [d, r] : s_replicas) cliPrintf("  %s hops %d flags %d\n", d.c_str(), r.hops, r.flags);
    cliPrintf("channel: %s %s\n", s_chanDest.empty() ? "-" : s_chanDest.c_str(),
              s_chan < 0 ? "closed" : s_chanUp ? "up" : "opening");
    cliPrintf("requests pending: %u, box size %u\n", (unsigned)s_client->pending(), (unsigned)s_client->box_size());
    cliPrintf("frames sent %u, refused %u, received %u; probe: %s\n", s_sent, s_sendFail, s_recv, s_probe.c_str());
    if (s_store) cliPrintf("saved: %u writes, %u bytes\n", (unsigned)s_store->writes, (unsigned)s_store->bytes);
}

// ---- task ----

static volatile bool s_stop = false, s_parked = false;

static void kqTaskMain(void*) {
    if (!itsServerInit()) err("kq itsServerInit failed");
    itsClientInit(4);
    storageSubscribeChanges("rnsd.chan.kq.rep", onChanState);
    loadChats();
    storageSubscribeChanges("kq.cmd.", onCmd);
    spawnTask(workerMain, "kq.io", kLightStack, nullptr, 1, 1, STACK_PSRAM);
    spawnTask(workerMain, "kq.ctidh", kHeavyStack, (void*)1, 1, 1, STACK_PSRAM);
    for (;;) {
        connectAnnounces();
        while (!s_stop) {
            itsPoll(pdMS_TO_TICKS(kTickMs));
            if (s_annHandle < 0) connectAnnounces();
            openChannel();
            s_client->tick(nowMs());
            pumpClient();
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
    storageEnd();
    s_client = new kq::ReplicaClient((size_t)storageGetInt("s.kq.box_size", 1549), sendFrame);
    s_mu = xSemaphoreCreateMutex();
    s_lightQ = xQueueCreate(1, sizeof(kq1::Action*));
    s_heavyQ = xQueueCreate(1, sizeof(kq1::Action*));
    s_doneQ = xQueueCreate(2, sizeof(Finished*));
    s_bridgeQ = xQueueCreate(2, sizeof(BridgeReq*));
    s_backend = new kq::ReplicaBackend(*s_client, bridge);
    s_rt = new kq::Runtime(kq1::State{storageGetStr("s.kq.name", "kq"), {}}, s_client->box_size());
    cliRegisterCmd("kq", cliKq);
    rnsServiceRegister(TAG, kqStart, kqStop);
}
