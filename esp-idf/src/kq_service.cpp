// The kq task: finds the nearest Pigeonhole replica that serves Channel mode, keeps one
// Channel to it, and carries the replica client's frames over it.
#include "kq_service.h"

#include <cstring>
#include <ctime>
#include <map>
#include <string>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ports.h"
#include "rnsd.h"
#include "spangap.h"

#include <sodium.h>

#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "kq_replica.h"
#include "kq_runtime.h"

static const char* TAG = "kq";
static constexpr char kAspect[] = "pigeonhole.replica";
static constexpr char kChanTag[] = "kq.rep";
static constexpr uint32_t kTickMs = 500, kReopenMs = 10000;
static constexpr size_t kAnnHeader = 1 + 16 + 16 + 64 + 32;

static uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

static std::string hexOf(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}

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
    uint8_t flags = n > kAnnHeader ? buf[kAnnHeader] : 0;
    auto it = s_replicas.find(dest);
    if (it != s_replicas.end() && memcmp(it->second.identity, buf + 17, 16) != 0) return;  // the first identity holds
    Replica r;
    memcpy(r.identity, buf + 17, 16);
    r.hops = buf[0];
    r.flags = flags;
    s_replicas[dest] = r;
    verb("kq: replica %s hops %d flags %d", dest.c_str(), r.hops, flags);
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

static kq::ReplicaClient* s_client = nullptr;
static TaskHandle_t s_task = nullptr;
static unsigned s_sent = 0, s_sendFail = 0, s_recv = 0;
static std::string s_probe = "-";

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

static constexpr int kWorkers = 2;
static kq::Runtime* s_rt = nullptr;
static kq::ReplicaBackend* s_backend = nullptr;
static SemaphoreHandle_t s_mu = nullptr;  // the engine's state, read by the CLI
static QueueHandle_t s_actionQ = nullptr, s_doneQ = nullptr, s_bridgeQ = nullptr;
static int s_idle = kWorkers;

struct BridgeReq {
    std::function<void(std::function<void()>)> fn;
    SemaphoreHandle_t sem;
};

static int64_t nowS() { return (int64_t)time(nullptr); }

static void wake() { if (s_task) xTaskNotifyGive(s_task); }

// On a worker: runs fn on the kq task, which owns the replica client, and waits for done.
static void bridge(std::function<void(std::function<void()>)> fn) {
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    auto* r = new BridgeReq{std::move(fn), sem};
    xQueueSend(s_bridgeQ, &r, portMAX_DELAY);
    wake();
    xSemaphoreTake(sem, portMAX_DELAY);
    vSemaphoreDelete(sem);
}

static void workerMain(void*) {
    kq1::Executor x{*s_backend, [](uint64_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }};
    for (;;) {
        kq1::Action* a = nullptr;
        if (xQueueReceive(s_actionQ, &a, portMAX_DELAY) != pdTRUE || !a) continue;
        auto* d = new kq1::Done(x.perform(*a));
        delete a;
        xQueueSend(s_doneQ, &d, portMAX_DELAY);
        wake();
    }
}

// On the kq task: what the workers asked for, what they finished, what is due, what is next.
static void pumpClient() {
    BridgeReq* r;
    while (xQueueReceive(s_bridgeQ, &r, 0) == pdTRUE) {
        SemaphoreHandle_t sem = r->sem;
        r->fn([sem] { xSemaphoreGive(sem); });
        delete r;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    kq1::Done* d;
    while (xQueueReceive(s_doneQ, &d, 0) == pdTRUE) {
        s_rt->finished(nowS(), nowMs(), *d);
        delete d;
        s_idle++;
    }
    s_rt->tick(nowS(), nowMs());
    while (s_idle > 0) {
        auto a = s_rt->take();
        if (!a) break;
        auto* p = new kq1::Action(std::move(*a));
        xQueueSend(s_actionQ, &p, portMAX_DELAY);
        s_idle--;
    }
    xSemaphoreGive(s_mu);
}

static void feed(const kq1::Input& in) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_rt->input(nowS(), nowMs(), in);
    xSemaphoreGive(s_mu);
}

// Commands reach the kq task as storage keys "kq.cmd.<verb>" = "<group>|<argument>".
static void onCmd(const char* key, const char* val) {
    if (!val || !*val) return;
    std::string verb = strrchr(key, '.') + 1, v = val;
    storageUnset(key);
    if (verb == "probe") { probe(); return; }
    size_t bar = v.find('|');
    std::string group = v.substr(0, bar), arg = bar == std::string::npos ? "" : v.substr(bar + 1);
    kq1::Input::Kind k = verb == "create" ? kq1::Input::CREATE : verb == "join" ? kq1::Input::JOIN
                       : verb == "invite" ? kq1::Input::INDUCT : verb == "send" ? kq1::Input::SEND
                       : verb == "scan" ? kq1::Input::SCAN : verb == "leave" ? kq1::Input::LEAVE : kq1::Input::CLEAR;
    if (k == kq1::Input::CLEAR) return;
    kq1::Input in(k);
    in.chat = group;
    in.name = group;
    in.code = arg;
    in.text = arg;
    feed(in);
}

// ---- CLI ----

static void probe() {
    uint8_t seed[32];
    esp_fill_random(seed, sizeof seed);
    size_t at = 0;
    auto w = kq::bacap::WriteCap::generate([&](uint8_t* p, size_t n) {
        while (n--) { if (at == sizeof seed) { esp_fill_random(seed, sizeof seed); at = 0; } *p++ = seed[at++]; }
    });
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

// ---- the client: the runtime on this task, the executor on workers ----

static constexpr int kWorkers = 2;
static kq::Runtime* s_rt = nullptr;
static kq::ReplicaBackend* s_backend = nullptr;
static SemaphoreHandle_t s_mu = nullptr;  // the engine's state, read by the CLI
static QueueHandle_t s_actionQ = nullptr, s_doneQ = nullptr, s_bridgeQ = nullptr;
static int s_idle = kWorkers;

struct BridgeReq {
    std::function<void(std::function<void()>)> fn;
    SemaphoreHandle_t sem;
};

static int64_t nowS() { return (int64_t)time(nullptr); }

static void wake() { if (s_task) xTaskNotifyGive(s_task); }

// On a worker: runs fn on the kq task, which owns the replica client, and waits for done.
static void bridge(std::function<void(std::function<void()>)> fn) {
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    auto* r = new BridgeReq{std::move(fn), sem};
    xQueueSend(s_bridgeQ, &r, portMAX_DELAY);
    wake();
    xSemaphoreTake(sem, portMAX_DELAY);
    vSemaphoreDelete(sem);
}

static void workerMain(void*) {
    kq1::Executor x{*s_backend, [](uint64_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }};
    for (;;) {
        kq1::Action* a = nullptr;
        if (xQueueReceive(s_actionQ, &a, portMAX_DELAY) != pdTRUE || !a) continue;
        auto* d = new kq1::Done(x.perform(*a));
        delete a;
        xQueueSend(s_doneQ, &d, portMAX_DELAY);
        wake();
    }
}

// On the kq task: what the workers asked for, what they finished, what is due, what is next.
static void pumpClient() {
    BridgeReq* r;
    while (xQueueReceive(s_bridgeQ, &r, 0) == pdTRUE) {
        SemaphoreHandle_t sem = r->sem;
        r->fn([sem] { xSemaphoreGive(sem); });
        delete r;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    kq1::Done* d;
    while (xQueueReceive(s_doneQ, &d, 0) == pdTRUE) {
        s_rt->finished(nowS(), nowMs(), *d);
        delete d;
        s_idle++;
    }
    s_rt->tick(nowS(), nowMs());
    while (s_idle > 0) {
        auto a = s_rt->take();
        if (!a) break;
        auto* p = new kq1::Action(std::move(*a));
        xQueueSend(s_actionQ, &p, portMAX_DELAY);
        s_idle--;
    }
    xSemaphoreGive(s_mu);
}

static void feed(const kq1::Input& in) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_rt->input(nowS(), nowMs(), in);
    xSemaphoreGive(s_mu);
}

// Commands reach the kq task as storage keys "kq.cmd.<verb>" = "<group>|<argument>".
static void onCmd(const char* key, const char* val) {
    if (!val || !*val) return;
    std::string verb = strrchr(key, '.') + 1, v = val;
    storageUnset(key);
    if (verb == "probe") { probe(); return; }
    size_t bar = v.find('|');
    std::string group = v.substr(0, bar), arg = bar == std::string::npos ? "" : v.substr(bar + 1);
    kq1::Input::Kind k = verb == "create" ? kq1::Input::CREATE : verb == "join" ? kq1::Input::JOIN
                       : verb == "invite" ? kq1::Input::INDUCT : verb == "send" ? kq1::Input::SEND
                       : verb == "scan" ? kq1::Input::SCAN : verb == "leave" ? kq1::Input::LEAVE : kq1::Input::CLEAR;
    if (k == kq1::Input::CLEAR) return;
    kq1::Input in(k);
    in.chat = group;
    in.name = group;
    in.code = arg;
    in.text = arg;
    feed(in);
}

// ---- CLI ----

static void probe() {
    uint8_t seed[32];
    esp_fill_random(seed, sizeof seed);
    size_t at = 0;
    auto w = kq::bacap::WriteCap::generate([&](uint8_t* p, size_t n) {
        while (n--) { if (at == sizeof seed) { esp_fill_random(seed, sizeof seed); at = 0; } *p++ = seed[at++]; }
    });
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

// Commands reach the kq task as storage keys, since its ITS handles are its own.
static void onCmdProbe(const char* key, const char* val) {
    if (!val || !*val) return;
    storageUnset(key);
    probe();
}

static void cliKq(const char* args) {
    std::string a = args ? args : "";
    if (!s_task) { cliPrintf("kq: not started (rnsd is not up yet)\n"); return; }
    if (a == "probe") { storageSet("kq.cmd.probe", "1"); cliPrintf("probe queued; `kq` shows the result\n"); return; }
    size_t sp = a.find(' ');
    std::string verb = a.substr(0, sp), rest = sp == std::string::npos ? "" : a.substr(sp + 1);
    sp = rest.find(' ');
    std::string group = rest.substr(0, sp), arg = sp == std::string::npos ? "" : rest.substr(sp + 1);
    if ((verb == "create" || verb == "join" || verb == "scan" || verb == "leave") && !group.empty()) {
        storageSet(("kq.cmd." + verb).c_str(), group.c_str());
        return;
    }
    if ((verb == "invite" || verb == "send") && !group.empty() && !arg.empty()) {
        storageSet(("kq.cmd." + verb).c_str(), (group + "|" + arg).c_str());
        return;
    }
    if (verb == "groups" || verb == "log") {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        for (auto& c : s_rt->eng.state.chats) {
            if (verb == "log" && c.id != group) continue;
            std::string st = c.group ? "joined" : "not joined";
            if (c.joining) st += c.joining->code.empty() ? ", minting a voucher" : ", voucher " + c.joining->code;
            if (c.inducting) st += ", adding a member";
            if (!c.jobs.empty()) st += ", " + std::to_string(c.jobs.size()) + " to write";
            cliPrintf("%s: %s, %u members\n", c.id.c_str(), st.c_str(), c.group ? (unsigned)c.group->readers.size() : 0u);
            if (verb != "log") continue;
            for (auto& l : c.log) {
                if (l.notice) { cliPrintf("  — %s\n", kq1::plain(*l.notice).c_str()); continue; }
                auto n = c.names.find(l.sender);
                bool mine = c.group && c.group->me() == l.sender;
                std::string who = mine ? "me" : n != c.names.end() ? n->second : "?";
                const char* mark = !mine ? "" : l.status == kq1::Status::Pending ? " …" : c.acknowledged(l) ? " ✓✓" : " ✓";
                cliPrintf("  %s%s: %s\n", who.c_str(), mark, l.text.c_str());
            }
        }
        xSemaphoreGive(s_mu);
        return;
    }
    if (!a.empty()) {
        cliPrintf("kq [status] | probe | groups | log <g> | create <g> | join <g> | invite <g> <code> | send <g> <text>\n");
        return;
    }
    cliPrintf("replicas heard: %u\n", (unsigned)s_replicas.size());
    for (auto& [d, r] : s_replicas) cliPrintf("  %s hops %d flags %d\n", d.c_str(), r.hops, r.flags);
    cliPrintf("channel: %s %s\n", s_chanDest.empty() ? "-" : s_chanDest.c_str(),
              s_chan < 0 ? "closed" : s_chanUp ? "up" : "opening");
    cliPrintf("requests pending: %u, box size %u\n", (unsigned)s_client->pending(), (unsigned)s_client->box_size());
    cliPrintf("frames sent %u, refused %u, received %u; probe: %s\n", s_sent, s_sendFail, s_recv, s_probe.c_str());
}

// ---- task ----

static volatile bool s_stop = false, s_parked = false;

static void kqTaskMain(void*) {
    if (!itsServerInit()) err("kq itsServerInit failed");
    itsClientInit(4);
    storageSubscribeChanges("rnsd.chan.kq.rep", onChanState);
    storageSubscribeChanges("kq.cmd.", onCmd);
    for (int i = 0; i < kWorkers; i++) spawnTask(workerMain, "kq.work", 32768, nullptr, 1, 1, STACK_PSRAM);
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
    s_actionQ = xQueueCreate(kWorkers, sizeof(kq1::Action*));
    s_doneQ = xQueueCreate(kWorkers, sizeof(kq1::Done*));
    s_bridgeQ = xQueueCreate(kWorkers, sizeof(BridgeReq*));
    s_backend = new kq::ReplicaBackend(*s_client, bridge);
    // shortcut: the state lives in RAM only; persisting it to /state is the next step.
    s_rt = new kq::Runtime(kq1::State{storageGetStr("s.kq.name", "kq"), {}}, s_client->box_size());
    cliRegisterCmd("kq", cliKq);
    rnsServiceRegister(TAG, kqStart, kqStop);
}
