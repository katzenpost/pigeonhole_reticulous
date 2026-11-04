#include "kq1_voucher.h"

#include <cstring>

#include <sodium.h>

#include "kq1_wire.h"

namespace kq1::voucher {

using kq::bacap::ReadCap;
using kq::bacap::WriteCap;

static std::array<uint8_t, 32> blake2b_256(const Bytes& b) {
    std::array<uint8_t, 32> h;
    crypto_generichash_blake2b(h.data(), 32, b.data(), b.size(), nullptr, 0);
    return h;
}

static Bytes sign(const WriteCap& w, const Bytes& msg) {
    uint8_t pk[32], sk[64];
    crypto_sign_ed25519_seed_keypair(pk, sk, w.seed);
    Bytes sig(64);
    crypto_sign_ed25519_detached(sig.data(), nullptr, msg.data(), msg.size(), sk);
    sodium_memzero(sk, sizeof sk);
    return sig;
}

// Canonical CBOR maps: keys ordered by length, then bytewise.
static Bytes map2(const char* k1, const Bytes& v1, bool v1_text, const char* k2, const Bytes& v2) {
    Bytes out;
    push_header(out, 5, 2);
    push_text(out, k1);
    if (v1_text) push_text(out, std::string(v1.begin(), v1.end()));
    else push_bytes(out, v1);
    push_text(out, k2);
    push_bytes(out, v2);
    return out;
}

static Bytes unsigned_v2(const Bytes& read_cap, const Bytes& pub, const std::string& name) {
    Bytes out;
    push_header(out, 4, 3);
    push_bytes(out, read_cap);
    push_bytes(out, pub);
    push_text(out, name);
    return out;
}

WriteCap derive_stream(const uint8_t voucher[32]) {
    kq::nike::Shake shake(voucher, 32);
    return WriteCap::generate(shake.rng());
}

int payload_form(const Bytes& p) {
    if (p.empty()) return 0;
    return p[0] >> 5 == 4 ? COMPACT : p[0] >> 5 == 5 ? LONG : 0;
}

std::optional<Minted> mint(const Bytes& message_write_cap, const std::string& name, int form,
                           const kq::nike::Rng& keypair_rng) {
    auto w = WriteCap::from(message_write_cap.data(), message_write_cap.size());
    if (!w || (form != COMPACT && form != LONG)) return std::nullopt;
    Bytes read_cap = w->read_cap().bytes();
    auto kp = kq::nike::generate(keypair_rng);
    Minted m;
    m.form = form;
    if (form == COMPACT) {
        Bytes u = unsigned_v2(read_cap, kp.pub, name), sig = sign(*w, u);
        m.payload.clear();
        push_header(m.payload, 4, 4);
        push_bytes(m.payload, read_cap);
        push_bytes(m.payload, kp.pub);
        push_text(m.payload, name);
        push_bytes(m.payload, sig);
    } else {
        Bytes pa = map2("DisplayName", Bytes(name.begin(), name.end()), true, "MessageReadCap", read_cap);
        Bytes spa = map2("PleaseAdd", pa, false, "Signature", sign(*w, pa));
        m.payload = map2("VoucherPubKey", kp.pub, false, "SignedPleaseAdd", spa);
    }
    m.voucher = blake2b_256(m.payload);
    m.stream = derive_stream(m.voucher.data());
    m.secret_key = kp.priv;
    m.public_key = kp.pub;
    return m;
}

static const Term* bytes_field(const Term& t, const char* k) {
    auto f = t.field(k);
    return f && f->kind == Term::BYTES ? f : nullptr;
}

std::optional<Inducted> induct(const uint8_t voucher[32], const Bytes& payload, const Bytes& who_reply,
                               const Bytes& salt, const kq::nike::Rng& seal_rng) {
    if (salt.size() != kSaltSize) return std::nullopt;
    auto h = blake2b_256(payload);
    if (sodium_memcmp(h.data(), voucher, 32) != 0) return std::nullopt;
    int form = payload_form(payload);
    auto t = Term::read(payload.data(), payload.size());
    if (!form || !t) return std::nullopt;
    Bytes read_cap, pub, signed_bytes, sig;
    std::string name;
    if (form == COMPACT) {
        if (t->kind != Term::LIST || t->items.size() != 4) return std::nullopt;
        auto& it = t->items;
        if (it[0].kind != Term::BYTES || it[1].kind != Term::BYTES || it[2].kind != Term::TEXT || it[3].kind != Term::BYTES)
            return std::nullopt;
        read_cap = it[0].bytes;
        pub = it[1].bytes;
        name = it[2].str();
        sig = it[3].bytes;
        signed_bytes = unsigned_v2(read_cap, pub, name);
    } else {
        auto spa_b = bytes_field(*t, "SignedPleaseAdd"), pub_b = bytes_field(*t, "VoucherPubKey");
        if (!spa_b || !pub_b) return std::nullopt;
        auto spa = Term::read(spa_b->bytes.data(), spa_b->bytes.size());
        if (!spa) return std::nullopt;
        auto pa_b = bytes_field(*spa, "PleaseAdd"), sig_b = bytes_field(*spa, "Signature");
        if (!pa_b || !sig_b) return std::nullopt;
        auto pa = Term::read(pa_b->bytes.data(), pa_b->bytes.size());
        if (!pa) return std::nullopt;
        auto cap_b = bytes_field(*pa, "MessageReadCap");
        auto name_t = pa->field("DisplayName");
        if (!cap_b || !name_t || name_t->kind != Term::TEXT) return std::nullopt;
        read_cap = cap_b->bytes;
        pub = pub_b->bytes;
        name = name_t->str();
        sig = sig_b->bytes;
        signed_bytes = pa_b->bytes;
    }
    auto rc = ReadCap::from(read_cap.data(), read_cap.size());
    if (!rc || sig.size() != 64 || !kq::ed_verify(rc->root_pk, signed_bytes.data(), signed_bytes.size(), sig.data()))
        return std::nullopt;
    if (!kq::nike::valid_public(pub)) return std::nullopt;
    Bytes plain;
    if (form == COMPACT) {
        push_header(plain, 4, 2);
        push_bytes(plain, who_reply);
        push_bytes(plain, salt);
    } else {
        plain = map2("WhoReply", who_reply, false, "VoucherSalt", salt);
    }
    Inducted r;
    r.form = form;
    r.name = name;
    r.sealed = form == COMPACT ? kq::nike::seal(pub, plain.data(), plain.size(), seal_rng)
                               : kq::nike::mkem_encapsulate(pub, plain.data(), plain.size(), seal_rng);
    if (r.sealed.empty()) return std::nullopt;
    ReadCap live = *rc;
    live.index = rc->index.mutate(salt.data(), salt.size());
    r.mutated_read_cap = live.bytes();
    r.salt = salt;
    return r;
}

static bool is_mkem(const Bytes& sealed) {
    if (sealed.empty() || sealed[0] >> 5 != 5) return false;
    auto t = Term::read(sealed.data(), sealed.size());
    return t && t->kind == Term::MAP && t->items.size() == 6 && t->field("EphemeralPublicKey") &&
           t->field("DEKCiphertexts") && t->field("Envelope");
}

std::optional<Opened> open_reply(const Bytes& secret_key, const Bytes& public_key, const Bytes& sealed,
                                 const Bytes& message_write_cap) {
    auto w = WriteCap::from(message_write_cap.data(), message_write_cap.size());
    if (!w) return std::nullopt;
    Opened o;
    if (is_mkem(sealed)) {
        auto pt = kq::nike::mkem_decapsulate(secret_key, sealed.data(), sealed.size());
        if (!pt) return std::nullopt;
        auto t = Term::read(pt->data(), pt->size());
        if (!t) return std::nullopt;
        auto who = bytes_field(*t, "WhoReply"), salt = bytes_field(*t, "VoucherSalt");
        if (!who || !salt) return std::nullopt;
        o.who_reply = who->bytes;
        o.salt = salt->bytes;
    } else {
        auto pt = kq::nike::open(secret_key, public_key, sealed.data(), sealed.size());
        if (!pt) return std::nullopt;
        auto t = Term::read(pt->data(), pt->size());
        if (!t || t->kind != Term::LIST || t->items.size() != 2 || t->items[0].kind != Term::BYTES ||
            t->items[1].kind != Term::BYTES || t->items[1].bytes.size() != kSaltSize)
            return std::nullopt;
        o.who_reply = t->items[0].bytes;
        o.salt = t->items[1].bytes;
    }
    WriteCap live = *w;
    live.index = w->index.mutate(o.salt.data(), o.salt.size());
    o.mutated_write_cap = live.bytes();
    return o;
}

std::vector<Bytes> reply_boxes(const Bytes& sealed, int form, size_t box_size) {
    if (form == LONG) return {sealed};
    std::vector<Bytes> out;
    size_t piece = box_size - 1;
    for (size_t off = 0; off < sealed.size(); off += piece) {
        size_t n = std::min(piece, sealed.size() - off);
        Bytes b{(uint8_t)(off + n < sealed.size() ? 'C' : 'F')};
        b.insert(b.end(), sealed.begin() + off, sealed.begin() + off + n);
        out.push_back(b);
    }
    return out;
}

}  // namespace kq1::voucher
