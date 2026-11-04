// The kq task: finds the nearest Pigeonhole replica that serves Channel mode, keeps one
// Channel to it, and carries the replica client's frames over it.
#include "kq_service.h"

#include <cstring>
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

#include "kq_replica.h"

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
    storageSubscribeChanges("kq.cmd.probe", onCmdProbe);
    for (;;) {
        connectAnnounces();
        while (!s_stop) {
            itsPoll(pdMS_TO_TICKS(kTickMs));
            if (s_annHandle < 0) connectAnnounces();
            openChannel();
            s_client->tick(nowMs());
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
    storageEnd();
    s_client = new kq::ReplicaClient((size_t)storageGetInt("s.kq.box_size", 1549), sendFrame);
    cliRegisterCmd("kq", cliKq);
    rnsServiceRegister(TAG, kqStart, kqStop);
}
