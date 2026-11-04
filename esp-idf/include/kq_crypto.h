// Primitives BACAP and the voucher seal need beyond what libsodium names directly.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kq {

using Bytes = std::vector<uint8_t>;

// HMAC and HKDF (RFC 2104, 5869) over unkeyed BLAKE2b of hash_len 64 or 32.
void hmac_blake2b(size_t hash_len, const uint8_t* key, size_t key_len,
                  const uint8_t* msg, size_t msg_len, uint8_t* out);
void hkdf_blake2b(size_t hash_len, const uint8_t* secret, size_t secret_len,
                  const uint8_t* salt, size_t salt_len,
                  const uint8_t* info, size_t info_len, uint8_t* out, size_t out_len);

void sha512_256(const uint8_t* in, size_t len, uint8_t out[32]);

// AES-256-GCM-SIV (RFC 8452). seal appends the 16 byte tag; open returns false on a bad tag.
Bytes gcmsiv_seal(const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t* aad, size_t aad_len, const uint8_t* pt, size_t pt_len);
bool gcmsiv_open(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t* aad, size_t aad_len, const uint8_t* ct, size_t ct_len, Bytes& pt);

// Blinded Ed25519 as hpqc/sign/ed25519/blinded25519.go.
bool ed_blind_public(const uint8_t pk[32], const uint8_t* factor, size_t factor_len, uint8_t out[32]);
// From the 32 byte seed: blinded secret scalar and its public key.
void ed_blind_private(const uint8_t seed[32], const uint8_t* factor, size_t factor_len,
                      uint8_t scalar[32], uint8_t pub[32]);
void ed_blinded_sign(const uint8_t scalar[32], const uint8_t pub[32],
                     const uint8_t* msg, size_t len, uint8_t sig[64]);
bool ed_verify(const uint8_t pk[32], const uint8_t* msg, size_t len, const uint8_t sig[64]);

}  // namespace kq
