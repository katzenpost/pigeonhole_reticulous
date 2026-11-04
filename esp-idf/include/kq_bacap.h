// SPDX-License-Identifier: AGPL-3.0-only
// BACAP, as hpqc/bacap: streams of boxes a write capability writes and a read capability reads.
#pragma once
#include <cstdint>
#include <array>
#include <functional>
#include <optional>

#include "kq_crypto.h"

namespace kq::bacap {

constexpr size_t kIndexSize = 104, kReadCapSize = 136, kWriteCapSize = 168;
constexpr const char kPigeonholeCtx[] = "pigeonhole context";

struct Index {
    uint64_t idx = 0;
    uint8_t blinding[32] = {}, enc_key[32] = {}, kdf_state[32] = {};

    Bytes bytes() const;
    static std::optional<Index> from(const uint8_t* p, size_t len);
    // nullopt when to is below idx.
    std::optional<Index> advance_to(uint64_t to) const;
    // nullopt at the last index.
    std::optional<Index> next() const;
    Index mutate(const uint8_t* ctx, size_t len) const;
};

// Reads n bytes of entropy.
using Rng = std::function<void(uint8_t*, size_t)>;

struct ReadCap {
    uint8_t root_pk[32] = {};
    Index index;
    Bytes bytes() const;
    static std::optional<ReadCap> from(const uint8_t* p, size_t len);
};

struct WriteCap {
    uint8_t seed[32] = {}, root_pk[32] = {};
    Index index;
    Bytes bytes() const;
    static std::optional<WriteCap> from(const uint8_t* p, size_t len);
    static WriteCap generate(const Rng& rng);
    ReadCap read_cap() const;
};

// Box ID at idx: the root key blinded by the index's factor, or by its HKDF under ctx.
std::optional<std::array<uint8_t, 32>> box_id(const uint8_t root_pk[32], const Index& idx);
std::optional<std::array<uint8_t, 32>> box_id(const uint8_t root_pk[32], const Index& idx,
                                              const uint8_t* ctx, size_t ctx_len);

struct Sealed {
    std::array<uint8_t, 32> box_id;
    Bytes ciphertext;
    std::array<uint8_t, 64> signature;
};
// An empty plaintext is sealed as given; a tombstone is sign_box over an empty ciphertext.
Sealed encrypt(const WriteCap& w, const Index& idx, const uint8_t* ctx, size_t ctx_len,
               const uint8_t* pt, size_t pt_len);
std::array<uint8_t, 64> sign_box(const WriteCap& w, const Index& idx, const uint8_t* ctx,
                                 size_t ctx_len, const uint8_t* ct, size_t ct_len);
// nullopt on a bad signature or tag; an empty ciphertext opens to an empty plaintext.
std::optional<Bytes> decrypt(const Index& idx, const uint8_t box[32], const uint8_t* ctx,
                             size_t ctx_len, const uint8_t* ct, size_t ct_len, const uint8_t sig[64]);

}  // namespace kq::bacap
