// What a client sends a Pigeonhole replica over Reticulum and what comes back: queries,
// replies, box padding, and the Channel frames that carry them on stacks without requests.
#pragma once
#include <array>
#include <map>
#include <optional>
#include <vector>

#include "kq_crypto.h"

namespace kq::wire {

enum Code : uint8_t {
    SUCCESS = 0, NOT_FOUND, INVALID_BOX_ID, INVALID_SIGNATURE, DATABASE_FAILURE, INVALID_PAYLOAD,
    STORAGE_FULL, INTERNAL_ERROR, INVALID_EPOCH, REPLICATION_FAILED, ALREADY_EXISTS, TOMBSTONE,
};

// A four byte big endian length, the data, zeros to box_size + 4. nullopt when data exceeds box_size.
std::optional<Bytes> pad(const uint8_t* data, size_t len, size_t box_size);
std::optional<Bytes> unpad(const uint8_t* padded, size_t len);

Bytes read_query(const uint8_t box_id[32]);
Bytes write_query(const uint8_t box_id[32], const uint8_t sig[64], const uint8_t* ct, size_t ct_len);

struct Reply {
    uint8_t code = INTERNAL_ERROR;
    bool found = false;  // SUCCESS with a box
    std::array<uint8_t, 64> signature{};
    Bytes ciphertext;
};
std::optional<Reply> decode_reply(const uint8_t* p, size_t len);

// ---- Channel mode ----

constexpr uint16_t kMsgType = 0x5048;
constexpr size_t kFrameHeader = 4, kMaxPieces = 255;
constexpr uint32_t kReassemblyTimeoutMs = 60000;

struct Frame {
    uint16_t request_id = 0;
    uint8_t index = 0, count = 1;
    Bytes data;
    Bytes pack() const;
    static std::optional<Frame> unpack(const uint8_t* p, size_t len);
};

// Link MDU = ⌊(MTU − 68) / 16⌋ × 16 − 1, Channel MDU = link MDU − 6, piece = Channel MDU − 4.
constexpr size_t piece_size(size_t mtu) { return (mtu - 68) / 16 * 16 - 1 - 6 - kFrameHeader; }

// Empty when data needs more than 255 pieces.
std::vector<Frame> split(uint16_t request_id, const uint8_t* data, size_t len, size_t piece);

class Reassembler {
public:
    explicit Reassembler(uint32_t timeout_ms = kReassemblyTimeoutMs) : timeout_ms_(timeout_ms) {}
    std::optional<Bytes> feed(const Frame& f, uint32_t now_ms);
    size_t pending() const { return pending_.size(); }

private:
    struct Pending {
        uint32_t started;
        uint8_t count;
        std::map<uint8_t, Bytes> pieces;
    };
    uint32_t timeout_ms_;
    std::map<uint16_t, Pending> pending_;
};

}  // namespace kq::wire
