// SPDX-License-Identifier: AGPL-3.0-only
// The hybrid NIKE X25519 then CTIDH-1024, and the hpqc sealed box and MKEM over it.
#pragma once
#include <functional>
#include <optional>
#include <string>

#include "kq1_wire.h"
#include "kq_crypto.h"

namespace kq::nike {

constexpr size_t kPublicKeySize = 32 + 128, kPrivateKeySize = 32 + 130, kSecretSize = 32 + 128;
// The voucher seal's name; X25519-CTIDH1024 is the same scheme under another name.
constexpr const char kSealName[] = "CTIDH1024-X25519";

using Rng = std::function<void(uint8_t*, size_t)>;

// How a seal, and opening one without the public key at hand, run their two CTIDH-1024 actions:
// both start from the one private key and neither feeds the other. in_turn runs a, then b;
// at_once runs b meanwhile on the other core (the ESP32-S3: a task pinned there, its stack in
// PSRAM; in turn when there is no memory for it) or on a thread. The results are the same.
using Fn = std::function<void()>;
void in_turn(const Fn& a, const Fn& b);
void at_once(const Fn& a, const Fn& b);
extern void (*both)(const Fn& a, const Fn& b);  // in_turn unless set

// SHAKE256(seed) as an entropy stream, as hpqc's shakeReader.
class Shake {
public:
    explicit Shake(const uint8_t* seed, size_t len);
    void operator()(uint8_t* out, size_t n);
    Rng rng() { return [this](uint8_t* o, size_t n) { (*this)(o, n); }; }
private:
    alignas(64) uint8_t st_[256];
};

struct Keypair {
    Bytes pub, priv;
};
// Reads the X25519 key's 32 bytes, then the CTIDH key's.
Keypair generate(const Rng& rng);
std::optional<Bytes> derive_secret(const Bytes& priv, const Bytes& pub);
// The public key of a private key: one X25519 and one CTIDH action. Empty when malformed.
Bytes public_of(const Bytes& priv);
bool valid_public(const Bytes& pub);

// hpqc protocols/sealedbox: ephemeral public key, then XSalsa20-Poly1305. An empty pub in open
// is derived beside the shared secret.
Bytes seal(const Bytes& recipient_pub, const uint8_t* pt, size_t len, const Rng& rng,
           const std::string& name = kSealName);
std::optional<Bytes> open(const Bytes& priv, const Bytes& pub, const uint8_t* ct, size_t len,
                          const std::string& name = kSealName);

// hpqc protocols/mkem, one recipient: the canonical CBOR map, and its trial decryption.
Bytes mkem_encapsulate(const Bytes& recipient_pub, const uint8_t* pt, size_t len, const Rng& rng);
std::optional<Bytes> mkem_decapsulate(const Bytes& priv, const uint8_t* ct, size_t len);

}  // namespace kq::nike
