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
#include <thread>

#include "kq_runtime.h"
#include "kq_view.h"
#include "sim.h"

namespace {

std::mutex g_mu;  // the runtime and the replica client
kq::ReplicaClient* g_client;
kq::Runtime* g_rt;
kq::ReplicaBackend* g_backend;
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

void kqLoop() {
    uint64_t lastTry = 0;
    for (;;) {
        {
            std::lock_guard<std::mutex> g(g_mu);
            if (g_sock < 0 && nowMs() - lastTry > 2000) { lastTry = nowMs(); connectSock(); }
            if (g_sock >= 0) readSock();
            g_client->tick((uint32_t)nowMs());
            g_rt->tick(nowS(), nowMs());
            while (auto a = g_rt->take_if([](const kq1::Action& x) { return (heavy(x) ? g_heavy : g_light).idle; })) {
                Worker& w = heavy(*a) ? g_heavy : g_light;
                w.idle = false;
                std::lock_guard<std::mutex> l(w.m);
                w.q.push_back(std::move(*a));
                w.cv.notify_one();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace

void simClientStart(const std::string& socketPath, const std::string& name, size_t boxSize) {
    if (sodium_init() < 0) { fprintf(stderr, "sim: sodium_init failed\n"); exit(1); }
    g_path = socketPath;
    g_client = new kq::ReplicaClient(boxSize, sendFrame);
    g_backend = new kq::ReplicaBackend(*g_client, bridge);
    g_rt = new kq::Runtime(kq1::State{name, {}}, boxSize);
    std::thread(workerMain, &g_light).detach();
    std::thread(workerMain, &g_heavy).detach();
    std::thread(kqLoop).detach();
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

void kqCommand(const char* verb, const std::string& group, const std::string& arg) {
    auto in = kq::input_of(verb, group, arg);
    if (!in) return;
    std::lock_guard<std::mutex> g(g_mu);
    g_rt->input(nowS(), nowMs(), *in);
}
