// SPDX-License-Identifier: AGPL-3.0-only
// Two clients through one in-memory replica, with real crypto: bob mints a voucher, alice adds
// him, they talk. Everything a device runs but the tasks that carry it.
#include <sodium.h>

#include "fake_replica.h"
#include "kq_runtime.h"
#include "vectors.h"

using namespace kq1;

struct Peer {
    std::string name;
    FakeReplica& rep;
    ReplicaClient client;
    ReplicaBackend backend;
    Runtime rt;
    Peer(const std::string& n, FakeReplica& r)
        : name(n), rep(r), client(1549, [this](const Bytes& f) { rep.on_frame(f); return true; }),
          backend(client, [this](auto fn) {
              bool fin = false;
              fn([&] { fin = true; });
              while (!fin && !rep.outbox.empty()) {
                  auto out = std::move(rep.outbox);
                  rep.outbox.clear();
                  for (auto& f : out) client.on_message(f.data(), f.size(), 0);
              }
          }),
          rt(State{n, {}}, 1549) {
        client.ready(0);
    }
    void in(Input x, uint64_t t) { rt.input((int64_t)(t / 1000), t, x); }
    // Runs every ready action; true when one ran.
    bool act(uint64_t t) {
        bool any = false;
        Executor x{backend, [](uint64_t) {}};
        while (auto a = rt.take()) {
            rt.finished((int64_t)(t / 1000), t, x.perform(*a));
            any = true;
        }
        return any;
    }
    const Chat* chat() const { return rt.eng.state.chat("g"); }
};

static Input make(Input::Kind k, const std::string& text = "") {
    Input x(k);
    x.chat = "g";
    x.name = "g";
    x.text = text;
    return x;
}

static void run(std::vector<Peer*> ms, uint64_t& t, uint64_t until) {
    for (;;) {
        bool any = false;
        for (auto* m : ms) any |= m->act(t);
        if (any) continue;
        std::optional<uint64_t> due;
        for (auto* m : ms)
            if (auto d = m->rt.next_due(); d && (!due || *d < *due)) due = d;
        if (!due || *due > until) return;
        t = std::max(t, *due);
        for (auto* m : ms) m->rt.tick((int64_t)(t / 1000), t);
    }
}

static bool says(const Peer& m, const std::string& text) {
    auto c = m.chat();
    if (!c) return false;
    for (auto& l : c->log) if (!l.notice && l.text == text) return true;
    return false;
}

int main(int, char**) {
    if (sodium_init() < 0) return 2;
    Vec v{"runtime", "alice and bob", {}};
    FakeReplica rep;
    Peer alice("alice", rep), bob("bob", rep);
    std::vector<Peer*> both{&alice, &bob};
    uint64_t t = 1000;

    alice.in(make(Input::CREATE), t);
    bob.in(make(Input::JOIN), t);
    run(both, t, t + 60000);
    check(alice.chat() && alice.chat()->group.has_value(), v, "alice founded g");
    auto code = bob.chat() && bob.chat()->joining ? bob.chat()->joining->code : "";
    check(!code.empty(), v, "bob has a voucher code");
    printf("bob's voucher code: %s\n", code.c_str());

    // As hushphone shows a code: in groups of eight, and pasted with its newline.
    std::string grouped;
    for (size_t i = 0; i < code.size(); i += 8) grouped += (i ? " " : "") + code.substr(i, 8);
    Input ind = make(Input::INDUCT);
    ind.code = grouped + "\n";
    alice.in(ind, t);
    run(both, t, t + 600000);
    check(bob.chat() && bob.chat()->group.has_value(), v, "bob joined");
    check(bob.chat() && !bob.chat()->joining, v, "bob no longer waiting");

    alice.in(make(Input::SEND, "hello bob"), t);
    bob.in(make(Input::SEND, "hello alice"), t);
    run(both, t, t + 600000);
    check(says(bob, "hello bob"), v, "bob read alice");
    check(says(alice, "hello alice"), v, "alice read bob");
    for (auto* m : both) {
        printf("%s:\n", m->name.c_str());
        if (m->chat())
            for (auto& l : m->chat()->log) printf("  %s\n", l.notice ? ("— " + plain(*l.notice)).c_str() : l.text.c_str());
    }
    printf("boxes on the replica: %zu\n", rep.boxes.size());
    return finish("runtime");
}
