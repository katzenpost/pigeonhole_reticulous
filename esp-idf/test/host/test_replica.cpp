// SPDX-License-Identifier: AGPL-3.0-only
// The Channel-mode replica client against an in-memory replica that answers as
// pigeonhole_rns does: stores verified boxes, refuses a different box under a used ID.
#include <algorithm>
#include <cstring>

#include <sodium.h>

#include "fake_replica.h"
#include "kq_probe.h"
#include "vectors.h"

using namespace kq;

static std::array<uint8_t, 32> id32(const Bytes& b) {
    std::array<uint8_t, 32> a;
    memcpy(a.data(), b.data(), 32);
    return a;
}

// Box sizes as pigeonhole_protocols testdata/box_sizes.json gives them: writes at the mesh size,
// a box too long for it at the smallest that holds it, and a stream of mixed sizes read.
static void sizes(const char* path) {
    for (auto& v : load(path, "pigeonhole/box_sizes.json")) {
        if (v.has("section") && v.s("section") == "sizes") {
            size_t n = v.u("plaintext");
            Json pad = Json::parse(v.s("pad"));
            auto padded = wire::pad(unhex(pad["plaintext"].s).data(), unhex(pad["plaintext"].s).size(), n);
            check(padded && *padded == unhex(pad["padded"].s) && padded->size() == v.u("padded"), v, "padded alike");
            FakeReplica rep;
            Bytes query;
            ReplicaClient c(wire::kBoxMesh, [&](const Bytes& f) { query.insert(query.end(), f.begin() + 4, f.end()); rep.on_frame(f); return true; });
            c.ready(0);
            uint8_t s = 3;
            auto w = bacap::WriteCap::generate([&](uint8_t* p, size_t k) { while (k--) *p++ = s++; });
            // The most the size holds; the mesh's first, then one byte past it, written at the next size.
            c.write(w, w.index, Bytes(n == wire::kBoxMesh ? n : wire::kBoxMesh + 1, 'a'), [](uint8_t) {});
            check(query.size() == v.u("write_query") && rep.boxes.begin()->second.second.size() == v.u("ciphertext") &&
                      query.size() + 4 + 6 == v.u("channel_message"), v, "written at this size");
        } else if (v.name == "mixed_stream") {
            FakeReplica rep;
            Json boxes = Json::parse(v.s("boxes")), plain = Json::parse(v.s("plaintexts"));
            std::vector<size_t> extra;
            for (auto& e : Json::parse(v.s("extra_sizes")).a) extra.push_back(e.u());
            for (auto& b : boxes.a) rep.boxes[id32(unhex(b["box_id"].s))] = {unhex(b["signature"].s), unhex(b["ciphertext"].s)};
            auto rc = v.hex("read_cap");
            for (int with = 0; with < 2; with++) {
                ReplicaClient c(wire::kBoxMesh, [&](const Bytes& f) { rep.on_frame(f); return true; },
                                with ? extra : std::vector<size_t>{});
                c.ready(0);
                size_t found = 0, refused = 0;
                for (size_t i = 0; i < boxes.a.size(); i++) {
                    auto idx = *bacap::Index::from(unhex(boxes.a[i]["index"].s).data(), bacap::kIndexSize);
                    ReadOutcome o;
                    c.read(rc.data(), idx, [&](const ReadOutcome& x) { o = x; });
                    for (auto& f : rep.outbox) c.on_message(f.data(), f.size(), 0);
                    rep.outbox.clear();
                    bool other = std::count(extra.begin(), extra.end(), (size_t)boxes.a[i]["size"].u());
                    found += o.kind == ReadOutcome::FOUND && o.plaintext == unhex(plain.a[i].s);
                    refused += !with && other && o.kind == ReadOutcome::FAILED;
                }
                check(with ? found == boxes.a.size() : found + refused == boxes.a.size() && refused > 0, v,
                      with ? "every box read, the configured size among them" : "a size not configured refused");
            }
        }
    }
    FakeReplica rep;
    ReplicaClient c(wire::kBoxMesh, [&](const Bytes& f) { rep.on_frame(f); return true; });
    c.ready(0);
    auto w = bacap::WriteCap::generate([](uint8_t* p, size_t k) { randombytes_buf(p, k); });
    Vec v{"replica", "sizes", {}};
    check(c.sizes() == std::vector<size_t>{256, 1549} && wire::allowed_sizes({512, 99, 20000}) == std::vector<size_t>{256, 512, 1549},
          v, "the allowed sizes, out of bounds ones left out");
    check(c.write(w, w.index, Bytes(1549, 1), [](uint8_t) {}) && !c.write(w, w.index, Bytes(1550, 1), [](uint8_t) {}), v,
          "1549 bytes fit the largest size, 1550 none");
}

// Push, against pigeonhole_rns's own encoding (vectors/pigeonhole/push_vectors.json): the
// subscribe as a query, its replies, and boxes pushed as Channel messages.
static void push(const char* path) {
    auto all = load(path, "pigeonhole/push_vectors.json");
    auto find = [&](const std::string& n) {
        for (auto& v : all) if (v.name == n) return v;
        fprintf(stderr, "no vector %s\n", n.c_str());
        exit(2);
    };
    Vec sub = find("subscribe"), replies = find("replies"), top = find("top");
    check(wire::subscribe_query(sub.hex("box_id").data()) == sub.hex("query"), sub, "the subscribe query");
    check(top.u("piece") == wire::piece_size(500), top, "the same piece size");
    auto rc = top.hex("read_cap");
    for (auto& v : all) {
        if (!v.has("section") || v.s("section") != "pushes") continue;
        auto idx = *bacap::Index::from(v.hex("index").data(), bacap::kIndexSize);
        std::vector<Bytes> sent;
        ReplicaClient c(wire::kBoxMesh, [&](const Bytes& f) { sent.push_back(f); return true; });
        c.ready(0);
        c.set_push(true);
        std::optional<Bytes> taken;
        bool called = false;
        c.subscribe(rc.data(), idx, [&](std::optional<Bytes> id) { taken = id; called = true; });
        auto q = wire::Frame::unpack(sent.back().data(), sent.back().size());
        check(q && q->data == wire::subscribe_query(v.hex("box_id").data()), v, "asked as pigeonhole_rns reads it");
        wire::Frame r{q->request_id, 0, 1, replies.hex("taken")};
        c.on_message(r.pack().data(), r.pack().size(), 0);
        check(called && taken == v.hex("box_id"), v, "taken: the Box ID handed back");
        bool msgtype = true;
        for (auto& m : Json::parse(v.s("channel_messages")).a) {
            Bytes b = unhex(m.s);
            msgtype &= ((uint16_t)(b[0] << 8 | b[1])) == wire::kPushMsgType;
            c.on_push(b.data() + 2, b.size() - 2, 0);
        }
        check(msgtype && c.take_woken() == std::vector<Bytes>{v.hex("box_id")} && c.pushes == 1, v, "pushed");
        size_t before = sent.size();
        ReadOutcome o;
        c.read(rc.data(), idx, [&](const ReadOutcome& x) { o = x; });
        bool tomb = v.u("size") == 0;
        check(sent.size() == before && (tomb ? o.kind == ReadOutcome::TOMBSTONE
                                             : o.kind == ReadOutcome::FOUND && o.plaintext == v.hex("plaintext")), v,
              "read from what was pushed, with no query");
        c.read(rc.data(), idx, [&](const ReadOutcome&) {});
        check(sent.size() == before + 1, v, "and once only");
    }
    // Refused, by a replica waiting for 256 already or one too old to know op 6; and no push to a
    // replica that announced none.
    for (const char* kind : {"refused", "invalid"}) {
        std::vector<Bytes> sent;
        ReplicaClient c(wire::kBoxMesh, [&](const Bytes& f) { sent.push_back(f); return true; });
        c.ready(0);
        c.set_push(true);
        Vec v = find("pushes/tombstone");
        auto idx = *bacap::Index::from(v.hex("index").data(), bacap::kIndexSize);
        bool called = false;
        std::optional<Bytes> taken;
        c.subscribe(rc.data(), idx, [&](std::optional<Bytes> id) { taken = id; called = true; });
        auto q = wire::Frame::unpack(sent.back().data(), sent.back().size());
        wire::Frame r{q->request_id, 0, 1, replies.hex(kind)};
        c.on_message(r.pack().data(), r.pack().size(), 0);
        for (auto& m : Json::parse(v.s("channel_messages")).a) {
            Bytes b = unhex(m.s);
            c.on_push(b.data() + 2, b.size() - 2, 0);
        }
        check(called && !taken && c.take_woken().empty() && c.pushes == 0, replies,
              (std::string(kind) + ": not subscribed, and a push of it not taken").c_str());
    }
    {
        size_t sent = 0;
        ReplicaClient c(wire::kBoxMesh, [&](const Bytes&) { sent++; return true; });
        c.ready(0);
        bool called = false;
        std::optional<Bytes> taken;
        auto idx = *bacap::Index::from(find("pushes/tombstone").hex("index").data(), bacap::kIndexSize);
        c.subscribe(rc.data(), idx, [&](std::optional<Bytes> id) { taken = id; called = true; });
        check(called && !taken && sent == 0, top, "a replica not announcing push is not asked");
    }

    // End to end, on a replica that pushes: one client waits, another writes.
    Vec v{"replica", "push", {}};
    FakeReplica rep;
    rep.push = true;
    ReplicaClient a(wire::kBoxMesh, [&](const Bytes& f) { rep.on_frame(f); return true; }),
        b(wire::kBoxMesh, [&](const Bytes& f) { rep.on_frame(f); return true; });
    a.ready(0);
    b.ready(0);
    a.set_push(true);
    auto deliver = [&] {
        auto out = std::move(rep.outbox), pushes = std::move(rep.pushbox);
        rep.outbox.clear();
        rep.pushbox.clear();
        for (auto& f : out) { a.on_message(f.data(), f.size(), 0); b.on_message(f.data(), f.size(), 0); }
        for (auto& f : pushes) a.on_push(f.data(), f.size(), 0);
    };
    auto w = bacap::WriteCap::generate([](uint8_t* p, size_t k) { randombytes_buf(p, k); });
    auto r = w.read_cap();
    std::optional<Bytes> id;
    a.subscribe(r.root_pk, r.index, [&](std::optional<Bytes> x) { id = x; });
    deliver();
    check(id && a.take_woken().empty(), v, "subscribed, nothing pushed yet");
    b.write(w, w.index, Bytes{'h', 'i'}, [](uint8_t) {});
    deliver();
    ReadOutcome o;
    a.read(r.root_pk, r.index, [&](const ReadOutcome& x) { o = x; });
    check(a.take_woken() == std::vector<Bytes>{*id} && o.kind == ReadOutcome::FOUND && o.plaintext == Bytes{'h', 'i'}, v,
          "pushed once written, and read from the push");
    a.subscribe(r.root_pk, r.index, [&](std::optional<Bytes> x) { id = x; });
    deliver();
    check(a.take_woken().size() == 1, v, "a box already held is pushed at once");
    auto next = *w.index.next();
    a.subscribe(r.root_pk, next, [&](std::optional<Bytes> x) { id = x; });
    deliver();
    a.reset();
    check(a.take_woken() == std::vector<Bytes>{*id}, v, "the Channel closing wakes what waited");
}

int main(int, char** argv) {
    if (sodium_init() < 0) return 2;
    sizes(argv[1]);
    push(argv[1]);
    Vec v{"replica", "client", {}};
    FakeReplica rep;
    size_t sent = 0;
    ReplicaClient c(1549, [&](const Bytes& f) { sent++; rep.on_frame(f); return true; });  // written at 1549
    auto deliver = [&] {
        auto out = std::move(rep.outbox);
        rep.outbox.clear();
        for (auto& f : out) c.on_message(f.data(), f.size(), 0);
    };
    uint8_t s = 7;
    auto w = bacap::WriteCap::generate([&](uint8_t* p, size_t n) { while (n--) *p++ = s++; });
    auto r = w.read_cap();
    std::vector<uint8_t> codes;
    auto wrote = [&](uint8_t code) { codes.push_back(code); };
    std::vector<ReadOutcome> reads;
    auto got = [&](const ReadOutcome& o) { reads.push_back(o); };

    Bytes msg{'h', 'i'};
    c.write(w, w.index, msg, wrote);
    check(sent == 0 && c.pending() == 1, v, "queued until the Channel is up");
    c.ready(0);
    check(sent == 4, v, "a 1549-byte box goes as four 421-byte pieces");
    deliver();
    check(codes == std::vector<uint8_t>{wire::SUCCESS}, v, "written");
    c.read(r.root_pk, r.index, got);
    deliver();
    check(reads.size() == 1 && reads[0].kind == ReadOutcome::FOUND && reads[0].plaintext == msg, v, "read back");
    c.write(w, w.index, Bytes{'x'}, wrote);
    deliver();
    check(codes.back() == wire::ALREADY_EXISTS, v, "another box under a used ID refused");
    auto next = *w.index.next();
    c.read(r.root_pk, next, got);
    deliver();
    check(reads.back().kind == ReadOutcome::NOT_FOUND, v, "not written yet");
    c.tombstone(w, next, wrote);
    deliver();
    c.read(r.root_pk, next, got);
    deliver();
    check(codes.back() == wire::SUCCESS && reads.back().kind == ReadOutcome::TOMBSTONE, v, "tombstone");
    check(!c.write(w, next, Bytes(1550, 1), wrote), v, "oversize refused");

    rep.answer = false;
    for (int i = 0; i < 3; i++) c.read(r.root_pk, r.index, got);
    check(c.pending() == 3, v, "two in flight and one queued");
    size_t before = reads.size();
    c.tick(ReplicaClient::kTimeoutMs - 1);
    check(reads.size() == before, v, "not yet timed out");
    c.tick(ReplicaClient::kTimeoutMs);
    check(reads.size() == before + 2 && reads.back().kind == ReadOutcome::FAILED && c.pending() == 1, v,
          "timed out; the queued one goes out");
    // Reads the replica sits on do not hold back a write: it has its own room.
    c.read(r.root_pk, r.index, got);
    c.read(r.root_pk, r.index, got);
    size_t sentBefore = sent, codesBefore = codes.size();
    c.write(w, *next.next(), Bytes{'w'}, wrote);
    check(sent == sentBefore + 4 && c.pending() == 4, v, "a write goes out past two reads in flight and one queued");
    c.reset();
    check(reads.size() == before + 5 && codes.size() == codesBefore + 1 && c.pending() == 0, v, "reset fails the rest");
    rep.answer = true;
    c.read(r.root_pk, r.index, got);
    check(c.pending() == 1, v, "held while down");
    c.ready(0);
    deliver();
    check(reads.back().kind == ReadOutcome::FOUND, v, "works again once up");

    // kq probe: each request answered 10 ms after it went out.
    uint32_t t = 0;
    Probe probe(c, [&] { return t; }, [](uint8_t* p, size_t n) { randombytes_buf(p, n); });
    std::string summary;
    check(probe.start(3, [&](const std::string& s) { summary = s; }) && !probe.start(1), v, "one probe at a time");
    for (int i = 0; i < 20 && probe.running(); i++) { t += 10; deliver(); }
    check(!probe.running() && probe.write.ms == std::vector<uint32_t>(3, 10) && probe.hit.ms == probe.write.ms &&
              probe.miss.ms == probe.write.ms, v, "three rounds of write, hit and miss, each timed");
    check(summary == "write 3: median 10 ms, min 10, max 10; hit 3: median 10 ms, min 10, max 10; "
                     "miss 3: median 10 ms, min 10, max 10", v, "summary");
    rep.answer = false;
    probe.start(1);
    c.tick(t + ReplicaClient::kTimeoutMs);
    check(!probe.running() && probe.write.failed == 1 && probe.status() == "write 0, 1 failed; hit 0; miss 0", v,
          "an unanswered write fails its round");
    return finish("replica");
}
