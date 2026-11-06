// SPDX-License-Identifier: AGPL-3.0-only
// A client of one Pigeonhole replica in Channel mode: box reads and writes become queries cut
// into frames, replies are matched to them by request id. No I/O: the owner hands it each
// Channel message that arrives and sends the frames it is given. Boxes are written at the mesh
// size and read at any allowed size; on a replica serving push, a box not yet written is
// subscribed to and pushed once it is.
#pragma once
#include <deque>
#include <functional>
#include <map>
#include <set>

#include "kq_bacap.h"
#include "kq_wire.h"

namespace kq {

struct ReadOutcome {
    enum Kind { FOUND, NOT_FOUND, TOMBSTONE, FAILED } kind = FAILED;
    Bytes plaintext;  // FOUND: the unpadded box contents
};

class ReplicaClient {
public:
    // Sends one Channel message of MSGTYPE wire::kMsgType; false when the Channel refused it.
    using Send = std::function<bool(const Bytes& frame)>;
    using WriteDone = std::function<void(uint8_t code)>;  // SUCCESS, ALREADY_EXISTS, or a failure code
    using ReadDone = std::function<void(const ReadOutcome&)>;

    // Reads and writes each have this many in flight, so that reads a replica is slow to miss
    // (the namenlos bridge takes up to 40 s) do not hold back a burst's writes.
    static constexpr size_t kMaxInFlight = 2;
    static constexpr uint32_t kTimeoutMs = 60000;

    // box_size: the size written; extra: sizes read besides wire::allowed_sizes' own.
    ReplicaClient(size_t box_size, Send send, std::vector<size_t> extra = {}, size_t piece = wire::piece_size(500));

    size_t box_size() const { return box_size_; }
    const std::vector<size_t>& sizes() const { return sizes_; }
    // false when the plaintext exceeds every allowed size.
    bool write(const bacap::WriteCap& w, const bacap::Index& idx, const Bytes& plaintext, WriteDone done);
    void tombstone(const bacap::WriteCap& w, const bacap::Index& idx, WriteDone done);
    void read(const uint8_t root_pk[32], const bacap::Index& idx, ReadDone done);

    // Whether the replica announced push (wire::kPushFlag).
    void set_push(bool on) { push_ = on; }
    bool push() const { return push_; }
    // Subscribes to the box at idx: done gets its Box ID once the replica took the subscription,
    // nothing when it cannot push. Once the box is pushed, or the Channel closes, that ID is in
    // take_woken(), and a read of a pushed box needs no query.
    void subscribe(const uint8_t root_pk[32], const bacap::Index& idx, std::function<void(std::optional<Bytes>)> done);
    std::vector<Bytes> take_woken();
    unsigned pushes = 0;  // pushes taken

    void on_message(const uint8_t* p, size_t n, uint32_t now_ms);
    // A Channel message of MSGTYPE wire::kPushMsgType.
    void on_push(const uint8_t* p, size_t n, uint32_t now_ms);
    // Fails requests unanswered for kTimeoutMs; they are asked again by their owner, later.
    void tick(uint32_t now_ms);
    // The Channel closed: every request fails, queued ones too.
    void reset();
    // The Channel is up: queued requests go out.
    void ready(uint32_t now_ms);
    size_t pending() const { return queue_.size() + in_flight_.size(); }

private:
    struct Req {
        bool write = false;
        Bytes query;
        std::function<void(const std::optional<wire::Reply>&)> on_reply;
        // A subscribe's reply is not a read's: raw, it goes to on_raw undecoded.
        bool raw = false;
        std::function<void(const std::optional<Bytes>&)> on_raw;
        uint32_t sent_at = 0;
        void answer(std::optional<Bytes> reply) {
            if (raw) on_raw(reply);
            else on_reply(reply ? wire::decode_reply(reply->data(), reply->size()) : std::nullopt);
        }
    };
    void enqueue(Req r);
    void pump(uint32_t now_ms);

    size_t box_size_, piece_;
    std::vector<size_t> sizes_;
    Send send_;
    bool up_ = false, push_ = false;
    std::set<Bytes> waiting_;          // Box IDs subscribed to
    std::map<Bytes, wire::Reply> pushed_;  // pushed and not yet read
    std::vector<Bytes> woken_;
    wire::Reassembler push_reasm_;
    uint16_t next_id_ = 1;
    uint32_t now_ = 0;
    std::deque<Req> queue_;
    std::map<uint16_t, Req> in_flight_;
    size_t flying(bool write) const;
    wire::Reassembler reasm_;
};

}  // namespace kq
