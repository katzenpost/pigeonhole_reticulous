// A client of one Pigeonhole replica in Channel mode: box reads and writes become queries cut
// into frames, replies are matched to them by request id. No I/O: the owner hands it each
// Channel message that arrives and sends the frames it is given.
#pragma once
#include <deque>
#include <functional>
#include <map>

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

    static constexpr size_t kMaxInFlight = 2;
    static constexpr uint32_t kTimeoutMs = 60000;

    ReplicaClient(size_t box_size, Send send, size_t piece = wire::piece_size(500))
        : box_size_(box_size), piece_(piece), send_(std::move(send)) {}

    size_t box_size() const { return box_size_; }
    // false when the plaintext exceeds the box size.
    bool write(const bacap::WriteCap& w, const bacap::Index& idx, const Bytes& plaintext, WriteDone done);
    void tombstone(const bacap::WriteCap& w, const bacap::Index& idx, WriteDone done);
    void read(const uint8_t root_pk[32], const bacap::Index& idx, ReadDone done);

    void on_message(const uint8_t* p, size_t n, uint32_t now_ms);
    // Fails requests unanswered for kTimeoutMs; they are asked again by their owner, later.
    void tick(uint32_t now_ms);
    // The Channel closed: every request fails, queued ones too.
    void reset();
    // The Channel is up: queued requests go out.
    void ready(uint32_t now_ms);
    size_t pending() const { return queue_.size() + in_flight_.size(); }

private:
    struct Req {
        Bytes query;
        std::function<void(const std::optional<wire::Reply>&)> on_reply;
        uint32_t sent_at = 0;
    };
    void enqueue(Req r);
    void pump(uint32_t now_ms);

    size_t box_size_, piece_;
    Send send_;
    bool up_ = false;
    uint16_t next_id_ = 1;
    uint32_t now_ = 0;
    std::deque<Req> queue_;
    std::map<uint16_t, Req> in_flight_;
    wire::Reassembler reasm_;
};

}  // namespace kq
