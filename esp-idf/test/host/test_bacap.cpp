// SPDX-License-Identifier: AGPL-3.0-only
// BACAP and its primitives against hpqc/bacap/testdata.
#include <sodium.h>

#include "kq_bacap.h"
#include "vectors.h"

using namespace kq;
using namespace kq::bacap;

static Index at(const WriteCap& w, uint64_t by) { return *w.index.advance_to(w.index.idx + by); }
static Bytes vec(const uint8_t* p, size_t n) { return Bytes(p, p + n); }

int main(int, char** argv) {
    const char* path = argv[1];
    if (sodium_init() < 0) return 2;

    for (auto& v : load(path, "bacap/blake2b_512.json")) {
        auto in = v.hex("input_hex");
        uint8_t out[64];
        crypto_generichash_blake2b(out, 64, in.data(), in.size(), nullptr, 0);
        check(vec(out, 64) == v.hex("output_hex"), v, "blake2b");
    }
    for (auto& v : load(path, "bacap/sha512.json")) {
        auto in = v.hex("input_hex");
        uint8_t out[64];
        crypto_hash_sha512(out, in.data(), in.size());
        check(vec(out, 64) == v.hex("output_hex"), v, "sha512");
    }
    for (auto& v : load(path, "bacap/sha512_256.json")) {
        auto in = v.hex("input_hex");
        uint8_t out[32];
        sha512_256(in.data(), in.size(), out);
        check(vec(out, 32) == v.hex("output_hex"), v, "sha512/256");
    }
    for (auto& v : load(path, "bacap/hkdf_blake2b.json")) {
        auto s = v.hex("secret_hex"), salt = v.hex("salt_hex"), info = v.hex("info_hex");
        Bytes out(v.u("length"));
        hkdf_blake2b(64, s.data(), s.size(), salt.data(), salt.size(), info.data(), info.size(), out.data(), out.size());
        check(out == v.hex("okm_hex"), v, "hkdf");
    }
    for (auto& v : load(path, "bacap/aes_gcm_siv.json")) {
        auto k = v.hex("key_hex"), n = v.hex("nonce_hex"), a = v.hex("aad_hex"), p = v.hex("plaintext_hex");
        auto c = gcmsiv_seal(k.data(), n.data(), a.data(), a.size(), p.data(), p.size());
        check(c == v.hex("ciphertext_hex"), v, "gcmsiv seal");
        Bytes back;
        check(gcmsiv_open(k.data(), n.data(), a.data(), a.size(), c.data(), c.size(), back) && back == p, v, "gcmsiv open");
        c[0] ^= 1;
        check(!gcmsiv_open(k.data(), n.data(), a.data(), a.size(), c.data(), c.size(), back), v, "gcmsiv tamper");
    }
    for (auto& v : load(path, "bacap/layout.json")) {
        auto wb = v.hex("writecap_hex");
        auto w = WriteCap::from(wb.data(), wb.size());
        check(w.has_value(), v, "parse writecap");
        if (!w) continue;
        check(w->bytes() == wb, v, "writecap roundtrip");
        check(vec(w->root_pk, 32) == v.hex("expected_root_public_key_hex"), v, "root pk");
        check(w->read_cap().bytes() == v.hex("expected_readcap_hex"), v, "readcap");
        check(w->index.bytes() == v.hex("expected_index_hex"), v, "index");
        check(w->index.idx == v.u("expected_idx64"), v, "idx64");
    }
    for (auto& v : load(path, "bacap/message_box_index.json")) {
        auto ib = v.hex("initial_index_hex");
        auto n = Index::from(ib.data(), ib.size())->advance_to(v.u("advance_to"));
        check(n && n->bytes() == v.hex("expected_index_hex"), v, "advance");
    }
    for (auto& v : load(path, "bacap/box_id.json")) {
        auto wb = v.hex("writecap_hex"), ctx = v.hex("ctx_hex");
        auto w = *WriteCap::from(wb.data(), wb.size());
        auto i = at(w, v.u("advance_by"));
        auto id = v.b("use_context") ? box_id(w.root_pk, i, ctx.data(), ctx.size()) : box_id(w.root_pk, i);
        check(id && vec(id->data(), 32) == v.hex("expected_box_id_hex"), v, "box id");
    }
    for (auto& v : load(path, "bacap/encrypt.json")) {
        auto wb = v.hex("writecap_hex"), ctx = v.hex("ctx_hex"), pt = v.hex("plaintext_hex");
        auto w = *WriteCap::from(wb.data(), wb.size());
        auto i = at(w, v.u("advance_by"));
        auto s = encrypt(w, i, ctx.data(), ctx.size(), pt.data(), pt.size());
        check(vec(s.box_id.data(), 32) == v.hex("expected_box_id_hex"), v, "box id");
        check(s.ciphertext == v.hex("expected_ciphertext_hex"), v, "ciphertext");
        check(vec(s.signature.data(), 64) == v.hex("expected_signature_hex"), v, "signature");
        auto r = w.read_cap();
        auto rid = box_id(r.root_pk, i, ctx.data(), ctx.size());
        check(rid && *rid == s.box_id, v, "reader box id");
        auto back = decrypt(i, s.box_id.data(), ctx.data(), ctx.size(), s.ciphertext.data(), s.ciphertext.size(), s.signature.data());
        check(back && *back == pt, v, "decrypt");
    }
    for (auto& v : load(path, "bacap/tombstone.json")) {
        auto wb = v.hex("writecap_hex"), ctx = v.hex("ctx_hex");
        auto w = *WriteCap::from(wb.data(), wb.size());
        auto i = at(w, v.u("advance_by"));
        auto sig = sign_box(w, i, ctx.data(), ctx.size(), nullptr, 0);
        auto id = box_id(w.root_pk, i, ctx.data(), ctx.size());
        check(vec(id->data(), 32) == v.hex("expected_box_id_hex"), v, "box id");
        check(vec(sig.data(), 64) == v.hex("expected_signature_hex"), v, "signature");
        auto back = decrypt(i, id->data(), ctx.data(), ctx.size(), nullptr, 0, sig.data());
        check(back && back->empty(), v, "open tombstone");
    }
    for (auto& v : load(path, "bacap/mutate_kdf_state.json")) {
        auto wb = v.hex("writecap_hex"), salt = v.hex("salt_hex"), ctx = v.hex("read_ctx_hex");
        auto w = *WriteCap::from(wb.data(), wb.size());
        auto m = at(w, v.u("advance_by")).mutate(salt.data(), salt.size());
        check(m.bytes() == v.hex("expected_mutated_index_hex"), v, "mutated index");
        auto id = box_id(w.root_pk, m, ctx.data(), ctx.size());
        check(id && vec(id->data(), 32) == v.hex("expected_mutated_box_id_hex"), v, "mutated box id");
    }
    for (auto& v : load(path, "bacap/position.json")) {
        auto rb = v.hex("readcap_hex"), ib = v.hex("index_hex");
        auto r = *ReadCap::from(rb.data(), rb.size());
        auto i = *Index::from(ib.data(), ib.size());
        auto adv = r.index.advance_to(i.idx);
        check((adv && adv->bytes() == ib) == v.b("reachable"), v, "reachable");
    }
    for (auto& v : load(path, "bacap/negative.json")) {
        const std::string& op = v.s("operation");
        bool rejected = false;
        if (op == "advance_index_to" || op == "next_index") {
            auto ib = v.hex("index_hex");
            auto i = *Index::from(ib.data(), ib.size());
            rejected = op == "next_index" ? !i.next() : !i.advance_to(v.u("advance_to"));
        } else if (op == "parse_message_box_index" || op == "parse_read_cap" || op == "parse_write_cap") {
            Bytes b = v.has("blob_hex") ? v.hex("blob_hex") : Bytes{};
            rejected = op == "parse_message_box_index" ? !Index::from(b.data(), b.size())
                     : op == "parse_read_cap"          ? !ReadCap::from(b.data(), b.size())
                                                       : !WriteCap::from(b.data(), b.size());
        } else {
            auto wb = v.hex("writecap_hex"), ctx = v.hex("ctx_hex"), id = v.hex("box_id_hex"), sig = v.hex("signature_hex");
            Bytes ct = v.has("ciphertext_hex") ? v.hex("ciphertext_hex") : Bytes{};
            auto w = *WriteCap::from(wb.data(), wb.size());
            auto i = at(w, v.u("advance_by"));
            if (op == "verify_box") {
                rejected = !ed_verify(id.data(), ct.data(), ct.size(), sig.data());
            } else if (op == "decrypt") {
                rejected = !decrypt(i, id.data(), ctx.data(), ctx.size(), ct.data(), ct.size(), sig.data());
            } else if (op == "open") {
                auto want = box_id(w.read_cap().root_pk, i, ctx.data(), ctx.size());
                rejected = !want || vec(want->data(), 32) != id ||
                           !decrypt(i, id.data(), ctx.data(), ctx.size(), ct.data(), ct.size(), sig.data());
            } else {
                check(false, v, "unknown operation");
                continue;
            }
        }
        check(rejected, v, v.s("category").c_str());
    }

    uint8_t seed = 0;
    auto rng = [&](uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = seed++; };
    auto w = WriteCap::generate(rng);
    auto again = WriteCap::from(w.bytes().data(), kWriteCapSize);
    Vec gen{"generate", "roundtrip", {}};
    check(again && again->bytes() == w.bytes(), gen, "generated cap parses back");
    return finish("bacap");
}
