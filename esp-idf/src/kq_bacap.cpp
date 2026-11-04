#include "kq_bacap.h"

#include <cstring>

#include <sodium.h>

namespace kq::bacap {

static const char kMutateLabel[] = "bacap-mutate-kdf-state-v1";

static void put_le64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

Bytes Index::bytes() const {
    Bytes b(kIndexSize);
    put_le64(b.data(), idx);
    memcpy(b.data() + 8, blinding, 32);
    memcpy(b.data() + 40, enc_key, 32);
    memcpy(b.data() + 72, kdf_state, 32);
    return b;
}

std::optional<Index> Index::from(const uint8_t* p, size_t len) {
    if (len != kIndexSize) return std::nullopt;
    Index m;
    m.idx = le64(p);
    memcpy(m.blinding, p + 8, 32);
    memcpy(m.enc_key, p + 40, 32);
    memcpy(m.kdf_state, p + 72, 32);
    return m;
}

std::optional<Index> Index::advance_to(uint64_t to) const {
    if (to < idx) return std::nullopt;
    if (to == idx) return *this;
    Index n;
    n.idx = idx;
    memcpy(n.kdf_state, kdf_state, 32);
    uint8_t info[8], okm[96];
    for (; n.idx < to; n.idx++) {
        put_le64(info, n.idx);
        hkdf_blake2b(64, n.kdf_state, 32, nullptr, 0, info, 8, okm, sizeof okm);
        memcpy(n.kdf_state, okm, 32);
        memcpy(n.enc_key, okm + 32, 32);
        memcpy(n.blinding, okm + 64, 32);
    }
    sodium_memzero(okm, sizeof okm);
    return n;
}

std::optional<Index> Index::next() const {
    if (idx == UINT64_MAX) return std::nullopt;
    return advance_to(idx + 1);
}

Index Index::mutate(const uint8_t* ctx, size_t len) const {
    Index n;
    n.idx = idx;
    uint8_t okm[96];
    hkdf_blake2b(64, kdf_state, 32, ctx, len, (const uint8_t*)kMutateLabel, sizeof kMutateLabel - 1,
                 okm, sizeof okm);
    memcpy(n.kdf_state, okm, 32);
    memcpy(n.enc_key, okm + 32, 32);
    memcpy(n.blinding, okm + 64, 32);
    sodium_memzero(okm, sizeof okm);
    return n;
}

Bytes ReadCap::bytes() const {
    Bytes b(root_pk, root_pk + 32), i = index.bytes();
    b.insert(b.end(), i.begin(), i.end());
    return b;
}

std::optional<ReadCap> ReadCap::from(const uint8_t* p, size_t len) {
    if (len != kReadCapSize) return std::nullopt;
    if (!crypto_core_ed25519_is_valid_point(p)) return std::nullopt;
    auto idx = Index::from(p + 32, kIndexSize);
    ReadCap r;
    memcpy(r.root_pk, p, 32);
    r.index = *idx;
    return r;
}

Bytes WriteCap::bytes() const {
    Bytes b(seed, seed + 32), i = index.bytes();
    b.insert(b.end(), root_pk, root_pk + 32);
    b.insert(b.end(), i.begin(), i.end());
    return b;
}

std::optional<WriteCap> WriteCap::from(const uint8_t* p, size_t len) {
    if (len != kWriteCapSize) return std::nullopt;
    WriteCap w;
    uint8_t sk[64];
    crypto_sign_ed25519_seed_keypair(w.root_pk, sk, p);
    sodium_memzero(sk, sizeof sk);
    if (sodium_memcmp(w.root_pk, p + 32, 32) != 0) return std::nullopt;
    memcpy(w.seed, p, 32);
    w.index = *Index::from(p + 64, kIndexSize);
    return w;
}

WriteCap WriteCap::generate(const Rng& rng) {
    WriteCap w;
    uint8_t sk[64], r[16];
    rng(w.seed, 32);
    crypto_sign_ed25519_seed_keypair(w.root_pk, sk, w.seed);
    sodium_memzero(sk, sizeof sk);
    Index m;
    rng(m.kdf_state, 32);
    rng(r, 16);
    r[7] &= 0x3f;
    r[15] &= 0x3f;
    m.idx = le64(r) + le64(r + 8);
    w.index = *m.next();
    return w;
}

ReadCap WriteCap::read_cap() const {
    ReadCap r;
    memcpy(r.root_pk, root_pk, 32);
    r.index = index;
    return r;
}

std::optional<std::array<uint8_t, 32>> box_id(const uint8_t root_pk[32], const Index& idx) {
    std::array<uint8_t, 32> out;
    if (!ed_blind_public(root_pk, idx.blinding, 32, out.data())) return std::nullopt;
    return out;
}

static void derive(const uint8_t key[32], const uint8_t* ctx, size_t ctx_len, uint8_t out[32]) {
    hkdf_blake2b(64, key, 32, ctx, ctx_len, nullptr, 0, out, 32);
}

std::optional<std::array<uint8_t, 32>> box_id(const uint8_t root_pk[32], const Index& idx,
                                              const uint8_t* ctx, size_t ctx_len) {
    uint8_t k[32];
    derive(idx.blinding, ctx, ctx_len, k);
    std::array<uint8_t, 32> out;
    if (!ed_blind_public(root_pk, k, 32, out.data())) return std::nullopt;
    return out;
}

static void blinded_key(const WriteCap& w, const Index& idx, const uint8_t* ctx, size_t ctx_len,
                        uint8_t scalar[32], uint8_t pub[32]) {
    uint8_t k[32];
    derive(idx.blinding, ctx, ctx_len, k);
    ed_blind_private(w.seed, k, 32, scalar, pub);
}

std::array<uint8_t, 64> sign_box(const WriteCap& w, const Index& idx, const uint8_t* ctx,
                                 size_t ctx_len, const uint8_t* ct, size_t ct_len) {
    uint8_t scalar[32], pub[32];
    blinded_key(w, idx, ctx, ctx_len, scalar, pub);
    std::array<uint8_t, 64> sig;
    ed_blinded_sign(scalar, pub, ct, ct_len, sig.data());
    sodium_memzero(scalar, sizeof scalar);
    return sig;
}

Sealed encrypt(const WriteCap& w, const Index& idx, const uint8_t* ctx, size_t ctx_len,
               const uint8_t* pt, size_t pt_len) {
    uint8_t scalar[32], e[32];
    Sealed s;
    blinded_key(w, idx, ctx, ctx_len, scalar, s.box_id.data());
    derive(idx.enc_key, ctx, ctx_len, e);
    s.ciphertext = gcmsiv_seal(e, s.box_id.data(), s.box_id.data(), 32, pt, pt_len);
    ed_blinded_sign(scalar, s.box_id.data(), s.ciphertext.data(), s.ciphertext.size(), s.signature.data());
    sodium_memzero(scalar, sizeof scalar);
    sodium_memzero(e, sizeof e);
    return s;
}

std::optional<Bytes> decrypt(const Index& idx, const uint8_t box[32], const uint8_t* ctx,
                             size_t ctx_len, const uint8_t* ct, size_t ct_len, const uint8_t sig[64]) {
    if (!ed_verify(box, ct, ct_len, sig)) return std::nullopt;
    if (ct_len == 0) return Bytes{};
    uint8_t e[32];
    derive(idx.enc_key, ctx, ctx_len, e);
    Bytes pt;
    bool ok = gcmsiv_open(e, box, box, 32, ct, ct_len, pt);
    sodium_memzero(e, sizeof e);
    if (!ok) return std::nullopt;
    return pt;
}

}  // namespace kq::bacap
