// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_crypto.h"

#include <cstring>

#include <mbedtls/aes.h>
#include <sodium.h>

namespace kq {

static constexpr size_t kBlake2bBlock = 128;

void hmac_blake2b(size_t hash_len, const uint8_t* key, size_t key_len,
                  const uint8_t* msg, size_t msg_len, uint8_t* out) {
    uint8_t k[kBlake2bBlock] = {0};
    if (key_len > kBlake2bBlock)
        crypto_generichash_blake2b(k, hash_len, key, key_len, nullptr, 0);
    else
        memcpy(k, key, key_len);
    uint8_t pad[kBlake2bBlock], inner[64];
    crypto_generichash_blake2b_state st;
    for (size_t i = 0; i < sizeof pad; i++) pad[i] = k[i] ^ 0x36;
    crypto_generichash_blake2b_init(&st, nullptr, 0, hash_len);
    crypto_generichash_blake2b_update(&st, pad, sizeof pad);
    crypto_generichash_blake2b_update(&st, msg, msg_len);
    crypto_generichash_blake2b_final(&st, inner, hash_len);
    for (size_t i = 0; i < sizeof pad; i++) pad[i] = k[i] ^ 0x5c;
    crypto_generichash_blake2b_init(&st, nullptr, 0, hash_len);
    crypto_generichash_blake2b_update(&st, pad, sizeof pad);
    crypto_generichash_blake2b_update(&st, inner, hash_len);
    crypto_generichash_blake2b_final(&st, out, hash_len);
    sodium_memzero(k, sizeof k);
    sodium_memzero(pad, sizeof pad);
}

void hkdf_blake2b(size_t hash_len, const uint8_t* secret, size_t secret_len,
                  const uint8_t* salt, size_t salt_len,
                  const uint8_t* info, size_t info_len, uint8_t* out, size_t out_len) {
    uint8_t zeros[64] = {0}, prk[64], t[64];
    if (salt_len == 0) { salt = zeros; salt_len = hash_len; }
    hmac_blake2b(hash_len, salt, salt_len, secret, secret_len, prk);
    Bytes m;
    size_t t_len = 0;
    for (uint8_t i = 1; out_len > 0; i++) {
        m.assign(t, t + t_len);
        m.insert(m.end(), info, info + info_len);
        m.push_back(i);
        hmac_blake2b(hash_len, prk, hash_len, m.data(), m.size(), t);
        t_len = hash_len;
        size_t n = out_len < hash_len ? out_len : hash_len;
        memcpy(out, t, n);
        out += n;
        out_len -= n;
    }
    sodium_memzero(prk, sizeof prk);
    sodium_memzero(t, sizeof t);
    sodium_memzero(m.data(), m.size());
}

void sha512_256(const uint8_t* in, size_t len, uint8_t out[32]) {
    // SHA-512 with the FIPS 180-4 §5.3.6.2 initial value, truncated.
    static const uint64_t iv[8] = {
        0x22312194FC2BF72CULL, 0x9F555FA3C84C64C2ULL, 0x2393B86B6F53B151ULL,
        0x963877195940EABDULL, 0x96283EE2A88EFFE3ULL, 0xBE5E1E2553863992ULL,
        0x2B0199FC2C85B8AAULL, 0x0EB72DDC81C52CA2ULL};
    crypto_hash_sha512_state st;
    crypto_hash_sha512_init(&st);
    memcpy(st.state, iv, sizeof iv);
    uint8_t full[64];
    crypto_hash_sha512_update(&st, in, len);
    crypto_hash_sha512_final(&st, full);
    memcpy(out, full, 32);
}

// ---- AES-256-GCM-SIV ----

namespace {

struct Aes {
    mbedtls_aes_context ctx;
    explicit Aes(const uint8_t* key, unsigned bits) {
        mbedtls_aes_init(&ctx);
        mbedtls_aes_setkey_enc(&ctx, key, bits);
    }
    ~Aes() { mbedtls_aes_free(&ctx); }
    void block(const uint8_t in[16], uint8_t out[16]) {
        mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    }
};

// POLYVAL field: little-endian bits, x^128 + x^127 + x^126 + x^121 + 1, dot(a,b) = a*b*x^-128.
// Computed as GHASH multiplication on byte-reversed operands (RFC 8452 Appendix A).
void ghash_mul(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16] = {0}, v[16];
    memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        uint8_t bit = (x[i / 8] >> (7 - i % 8)) & 1;
        uint8_t m = (uint8_t)-bit;
        for (int j = 0; j < 16; j++) z[j] ^= v[j] & m;
        uint8_t lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        v[0] ^= 0xe1 & (uint8_t)-lsb;
    }
    memcpy(x, z, 16);
}

void reverse16(uint8_t* b) {
    for (int i = 0; i < 8; i++) { uint8_t t = b[i]; b[i] = b[15 - i]; b[15 - i] = t; }
}

struct Polyval {
    uint8_t h[16], s[16] = {0};
    explicit Polyval(const uint8_t key[16]) {
        // mulX_GHASH(ByteReverse(H))
        memcpy(h, key, 16);
        reverse16(h);
        uint8_t lsb = h[15] & 1;
        for (int j = 15; j > 0; j--) h[j] = (uint8_t)((h[j] >> 1) | (h[j - 1] << 7));
        h[0] >>= 1;
        h[0] ^= 0xe1 & (uint8_t)-lsb;
    }
    void block(const uint8_t b[16]) {
        uint8_t r[16];
        memcpy(r, b, 16);
        reverse16(r);
        for (int i = 0; i < 16; i++) s[i] ^= r[i];
        ghash_mul(s, h);
    }
    void padded(const uint8_t* p, size_t n) {
        for (; n >= 16; p += 16, n -= 16) block(p);
        if (n) { uint8_t b[16] = {0}; memcpy(b, p, n); block(b); }
    }
    void out(uint8_t o[16]) { memcpy(o, s, 16); reverse16(o); }
};

void put_le64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

// Derives the keys and computes the tag over (aad, pt). Returns the encryption key's Aes.
void siv_tag(Aes& kgk, const uint8_t nonce[12], uint8_t enc_key[32], const uint8_t* aad,
             size_t aad_len, const uint8_t* pt, size_t pt_len, uint8_t tag[16]) {
    uint8_t auth_key[16], in[16] = {0}, out[16];
    memcpy(in + 4, nonce, 12);
    for (uint32_t i = 0; i < 6; i++) {
        in[0] = (uint8_t)i;
        kgk.block(in, out);
        memcpy(i < 2 ? auth_key + 8 * i : enc_key + 8 * (i - 2), out, 8);
    }
    Polyval pv(auth_key);
    pv.padded(aad, aad_len);
    pv.padded(pt, pt_len);
    uint8_t len_block[16];
    put_le64(len_block, (uint64_t)aad_len * 8);
    put_le64(len_block + 8, (uint64_t)pt_len * 8);
    pv.block(len_block);
    uint8_t s[16];
    pv.out(s);
    for (int i = 0; i < 12; i++) s[i] ^= nonce[i];
    s[15] &= 0x7f;
    Aes(enc_key, 256).block(s, tag);
    sodium_memzero(auth_key, sizeof auth_key);
}

void siv_ctr(const uint8_t enc_key[32], const uint8_t tag[16], const uint8_t* in, size_t n, uint8_t* out) {
    Aes aes(enc_key, 256);
    uint8_t ctr[16], ks[16];
    memcpy(ctr, tag, 16);
    ctr[15] |= 0x80;
    for (size_t off = 0; off < n; off += 16) {
        aes.block(ctr, ks);
        for (size_t i = 0; i < 16 && off + i < n; i++) out[off + i] = in[off + i] ^ ks[i];
        uint32_t c = (uint32_t)ctr[0] | (uint32_t)ctr[1] << 8 | (uint32_t)ctr[2] << 16 | (uint32_t)ctr[3] << 24;
        c++;
        for (int i = 0; i < 4; i++) ctr[i] = (uint8_t)(c >> (8 * i));
    }
}

}  // namespace

Bytes gcmsiv_seal(const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t* aad, size_t aad_len, const uint8_t* pt, size_t pt_len) {
    Aes kgk(key, 256);
    uint8_t enc_key[32], tag[16];
    siv_tag(kgk, nonce, enc_key, aad, aad_len, pt, pt_len, tag);
    Bytes out(pt_len + 16);
    siv_ctr(enc_key, tag, pt, pt_len, out.data());
    memcpy(out.data() + pt_len, tag, 16);
    sodium_memzero(enc_key, sizeof enc_key);
    return out;
}

bool gcmsiv_open(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t* aad, size_t aad_len, const uint8_t* ct, size_t ct_len, Bytes& pt) {
    if (ct_len < 16) return false;
    size_t n = ct_len - 16;
    const uint8_t* tag = ct + n;
    Aes kgk(key, 256);
    uint8_t enc_key[32], in[16] = {0}, out[16];
    memcpy(in + 4, nonce, 12);
    for (uint32_t i = 2; i < 6; i++) {
        in[0] = (uint8_t)i;
        kgk.block(in, out);
        memcpy(enc_key + 8 * (i - 2), out, 8);
    }
    pt.resize(n);
    siv_ctr(enc_key, tag, ct, n, pt.data());
    uint8_t expect[16];
    siv_tag(kgk, nonce, enc_key, aad, aad_len, pt.data(), n, expect);
    sodium_memzero(enc_key, sizeof enc_key);
    if (sodium_memcmp(expect, tag, 16) != 0) {
        sodium_memzero(pt.data(), n);
        pt.clear();
        return false;
    }
    return true;
}

// ---- blinded Ed25519 ----

namespace {

// clamp(x) mod L, as edwards25519.Scalar.SetBytesWithClamping.
void clamped_scalar(const uint8_t in[32], uint8_t out[32]) {
    uint8_t wide[64] = {0};
    memcpy(wide, in, 32);
    wide[0] &= 248;
    wide[31] &= 127;
    wide[31] |= 64;
    crypto_core_ed25519_scalar_reduce(out, wide);
    sodium_memzero(wide, sizeof wide);
}

void factor_scalar(const uint8_t* factor, size_t len, uint8_t out[32]) {
    uint8_t h[32];
    sha512_256(factor, len, h);
    clamped_scalar(h, out);
}

}  // namespace

bool ed_blind_public(const uint8_t pk[32], const uint8_t* factor, size_t factor_len, uint8_t out[32]) {
    if (factor_len == 0) return false;
    uint8_t f[32];
    factor_scalar(factor, factor_len, f);
    return crypto_scalarmult_ed25519_noclamp(out, f, pk) == 0;
}

void ed_blind_private(const uint8_t seed[32], const uint8_t* factor, size_t factor_len,
                      uint8_t scalar[32], uint8_t pub[32]) {
    uint8_t h[64], a[32], f[32];
    crypto_hash_sha512(h, seed, 32);
    clamped_scalar(h, a);
    factor_scalar(factor, factor_len, f);
    crypto_core_ed25519_scalar_mul(scalar, f, a);
    crypto_scalarmult_ed25519_base_noclamp(pub, scalar);
    sodium_memzero(h, sizeof h);
    sodium_memzero(a, sizeof a);
}

void ed_blinded_sign(const uint8_t scalar[32], const uint8_t pub[32],
                     const uint8_t* msg, size_t len, uint8_t sig[64]) {
    uint8_t d[64], r_wide[64], r[32], k_wide[64], k[32];
    crypto_hash_sha512(d, scalar, 32);
    crypto_hash_sha512_state st;
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, d + 32, 32);
    crypto_hash_sha512_update(&st, msg, len);
    crypto_hash_sha512_update(&st, d + 33, 31);
    crypto_hash_sha512_final(&st, r_wide);
    crypto_core_ed25519_scalar_reduce(r, r_wide);
    crypto_scalarmult_ed25519_base_noclamp(sig, r);
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, sig, 32);
    crypto_hash_sha512_update(&st, pub, 32);
    crypto_hash_sha512_update(&st, msg, len);
    crypto_hash_sha512_final(&st, k_wide);
    crypto_core_ed25519_scalar_reduce(k, k_wide);
    uint8_t ka[32];
    crypto_core_ed25519_scalar_mul(ka, k, scalar);
    crypto_core_ed25519_scalar_add(sig + 32, ka, r);
    sodium_memzero(d, sizeof d);
    sodium_memzero(r_wide, sizeof r_wide);
    sodium_memzero(r, sizeof r);
}

bool ed_verify(const uint8_t pk[32], const uint8_t* msg, size_t len, const uint8_t sig[64]) {
    return crypto_sign_ed25519_verify_detached(sig, msg, len, pk) == 0;
}

}  // namespace kq
