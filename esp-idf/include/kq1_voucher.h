// Contact vouchers, as hpqc/voucher: the compact form (2), a CBOR array answered with a sealed
// box, and the long form (1), signed maps answered with the MKEM.
#pragma once
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "kq_bacap.h"
#include "kq_nike.h"

namespace kq1::voucher {

using kq::Bytes;
constexpr int COMPACT = 2, LONG = 1;
constexpr size_t kSaltSize = 32;

struct Minted {
    int form;
    std::array<uint8_t, 32> voucher;
    Bytes payload;  // box 0 of the voucher stream
    kq::bacap::WriteCap stream;
    Bytes secret_key, public_key;
};
// keypair_rng draws the seal keypair: random on a device, SHAKE256(seed) for vectors.
std::optional<Minted> mint(const Bytes& message_write_cap, const std::string& name, int form,
                           const kq::nike::Rng& keypair_rng);

// The voucher stream's write capability: BACAP from SHAKE256(voucher).
kq::bacap::WriteCap derive_stream(const uint8_t voucher[32]);
// The payload's form, from its first byte: 2 an array, 1 a map, 0 neither.
int payload_form(const Bytes& payload);

struct Inducted {
    int form;
    std::string name;
    Bytes mutated_read_cap, sealed, salt;
};
// Checks the payload against the voucher and its signature, then seals [who_reply, salt].
std::optional<Inducted> induct(const uint8_t voucher[32], const Bytes& payload, const Bytes& who_reply,
                               const Bytes& salt, const kq::nike::Rng& seal_rng);

struct Opened {
    Bytes who_reply, salt, mutated_write_cap;
};
// public_key is the seal key minted beside secret_key; deriving it would cost a CTIDH action.
std::optional<Opened> open_reply(const Bytes& secret_key, const Bytes& public_key, const Bytes& sealed,
                                 const Bytes& message_write_cap);

// The boxes from box 1 on: the compact form in pieces of box_size - 1 behind C, the last behind F;
// the long form unframed, in box 1 alone.
std::vector<Bytes> reply_boxes(const Bytes& sealed, int form, size_t box_size);

}  // namespace kq1::voucher
