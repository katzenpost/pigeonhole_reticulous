// The Channel-mode replica client against an in-memory replica that answers as
// pigeonhole_rns does: stores verified boxes, refuses a different box under a used ID.
#include <cstring>

#include <sodium.h>

#include "kq_replica.h"
#include "vectors.h"

using namespace kq;

struct FakeReplica {
    std::map<std::array<uint8_t, 32>, std::pair<Bytes, Bytes>> boxes;  // id -> (sig, ct); empty ct a tombstone
    wire::Reassembler reasm;
    std::vector<Bytes> outbox;  // frames for the client
    bool answer = true;

    void on_frame(const Bytes& f) {
        auto fr = wire::Frame::unpack(f.data(), f.size());
        auto q = reasm.feed(*fr, 0);
        if (!q || !answer) return;
        Bytes reply;
        std::array<uint8_t, 32> id;
        memcpy(id.data(), q->data() + 1, 32);
        if ((*q)[0] == 0) {
            auto it = boxes.find(id);
            if (it == boxes.end()) reply = {wire::NOT_FOUND};
            else if (it->second.second.empty()) reply = {wire::TOMBSTONE};
            else {
                reply = {wire::SUCCESS};
                reply.insert(reply.end(), it->second.first.begin(), it->second.first.end());
                reply.insert(reply.end(), it->second.second.begin(), it->second.second.end());
            }
        } else {
            Bytes sig(q->begin() + 33, q->begin() + 97), ct(q->begin() + 97, q->end());
            auto it = boxes.find(id);
            if (!ed_verify(id.data(), ct.data(), ct.size(), sig.data())) reply = {wire::INVALID_SIGNATURE};
            else if (it != boxes.end() && it->second.second.empty()) reply = {wire::ALREADY_EXISTS};
            else if (it != boxes.end() && !ct.empty() && it->second != std::make_pair(sig, ct)) reply = {wire::ALREADY_EXISTS};
            else { boxes[id] = {sig, ct}; reply = {wire::SUCCESS}; }
        }
        for (auto& r : wire::split(fr->request_id, reply.data(), reply.size(), 900)) outbox.push_back(r.pack());
    }
};

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
    c.reset();
    check(reads.size() == before + 3 && c.pending() == 0, v, "reset fails the rest");
    rep.answer = true;
    c.read(r.root_pk, r.index, got);
    check(c.pending() == 1, v, "held while down");
    c.ready(0);
    deliver();
    check(reads.back().kind == ReadOutcome::FOUND, v, "works again once up");
    return finish("replica");
}
