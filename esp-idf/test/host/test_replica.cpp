// SPDX-License-Identifier: AGPL-3.0-only
// The Channel-mode replica client against an in-memory replica that answers as
// pigeonhole_rns does: stores verified boxes, refuses a different box under a used ID.
#include <cstring>

#include <sodium.h>

#include "fake_replica.h"
#include "kq_probe.h"
#include "vectors.h"

using namespace kq;

int main(int, char**) {
    if (sodium_init() < 0) return 2;
    Vec v{"replica", "client", {}};
    FakeReplica rep;
    size_t sent = 0;
    ReplicaClient c(1549, [&](const Bytes& f) { sent++; rep.on_frame(f); return true; });
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
