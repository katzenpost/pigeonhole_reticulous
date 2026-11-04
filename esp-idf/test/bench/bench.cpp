// SPDX-License-Identifier: AGPL-3.0-only
// How long each of the client's operations takes, slowest first, and what the things a user
// does cost, each as the operations it is made of. Times are of this machine: the order, and
// the gaps between, are what carry over to a device, not the numbers.
#include <mbedtls/aes.h>
#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "kq1_voucher.h"
#include "kq1_wire.h"
#include "kq_bacap.h"
#include "kq_crypto.h"
#include "kq_json.h"
#include "kq_nike.h"
#include "kq_store.h"
#include "kq_vault.h"
#include "kq_wire.h"

using kq::Bytes;
namespace bacap = kq::bacap;
namespace nike = kq::nike;
namespace wire = kq::wire;

static constexpr size_t kBox = 1549;  // namenlos
static const uint8_t* ctx() { return (const uint8_t*)bacap::kPigeonholeCtx; }
static constexpr size_t kCtxLen = sizeof bacap::kPigeonholeCtx - 1;
static void rnd(uint8_t* p, size_t n) { randombytes_buf(p, n); }

struct Result {
    std::string name;
    double us;
    size_t reps;
};
static std::vector<Result> g_results;
static double g_budget = 0.3;  // seconds per operation

// Runs f until the budget is spent (at least 3 times), and keeps the median time of a run.
static double bench(const std::string& name, const std::function<void()>& f) {
    std::vector<double> t;
    auto start = std::chrono::steady_clock::now();
    do {
        auto t0 = std::chrono::steady_clock::now();
        f();
        t.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    } while (t.size() < 3 || std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < g_budget);
    std::sort(t.begin(), t.end());
    double med = t[t.size() / 2];
    g_results.push_back({name, med, t.size()});
    fprintf(stderr, "  %-44s %12.1f us\n", name.c_str(), med);
    return med;
}

static double us(const std::string& name) {
    for (auto& r : g_results) if (r.name == name) return r.us;
    return 0;
}

static std::string fmt(double us) {
    char b[32];
    if (us >= 1e6) snprintf(b, sizeof b, "%.2f s", us / 1e6);
    else if (us >= 1e3) snprintf(b, sizeof b, "%.2f ms", us / 1e3);
    else snprintf(b, sizeof b, "%.1f µs", us);
    return b;
}

int main(int argc, char** argv) {
    if (sodium_init() < 0) return 2;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--budget")) g_budget = atof(argv[i + 1]);
    fprintf(stderr, "operations:\n");

    // ---- CTIDH-1024 with X25519: the NIKE under vouchers ----
    auto kp = nike::generate(rnd);
    auto kp2 = nike::generate(rnd);
    bench("nike: keypair (CTIDH-1024 + X25519)", [] { nike::generate(rnd); });
    bench("nike: shared secret (CTIDH-1024 + X25519)", [&] { nike::derive_secret(kp.priv, kp2.pub); });
    bench("nike: public key of a private key", [&] { nike::public_of(kp.priv); });
    bench("nike: public key is valid", [&] { nike::valid_public(kp2.pub); });
    Bytes msg(400, 7);
    Bytes sealedbox = nike::seal(kp.pub, msg.data(), msg.size(), rnd);
    bench("nike: sealed box, seal 400 B", [&] { nike::seal(kp.pub, msg.data(), msg.size(), rnd); });
    bench("nike: sealed box, open 400 B", [&] { nike::open(kp.priv, kp.pub, sealedbox.data(), sealedbox.size()); });
    Bytes mk = nike::mkem_encapsulate(kp.pub, msg.data(), msg.size(), rnd);
    bench("nike: MKEM, encapsulate 400 B", [&] { nike::mkem_encapsulate(kp.pub, msg.data(), msg.size(), rnd); });
    bench("nike: MKEM, decapsulate 400 B", [&] { nike::mkem_decapsulate(kp.priv, mk.data(), mk.size()); });

    // ---- BACAP: every box written and read ----
    auto w = bacap::WriteCap::generate(rnd);
    auto r = w.read_cap();
    Bytes text(200, 'a');
    auto padded = *wire::pad(text.data(), text.size(), kBox);
    auto box = bacap::encrypt(w, w.index, ctx(), kCtxLen, padded.data(), padded.size());
    bench("bacap: write capability, generate", [] { bacap::WriteCap::generate(rnd); });
    bench("bacap: box ID (blind the root key)", [&] { bacap::box_id(r.root_pk, r.index, ctx(), kCtxLen); });
    bench("bacap: encrypt and sign a box", [&] { bacap::encrypt(w, w.index, ctx(), kCtxLen, padded.data(), padded.size()); });
    bench("bacap: sign a tombstone", [&] { bacap::sign_box(w, w.index, ctx(), kCtxLen, nullptr, 0); });
    if (!bacap::decrypt(r.index, box.box_id.data(), ctx(), kCtxLen, box.ciphertext.data(), box.ciphertext.size(),
                        box.signature.data())) { fprintf(stderr, "bench: the box does not open\n"); return 1; }
    bench("bacap: verify and decrypt a box", [&] {
        bacap::decrypt(r.index, box.box_id.data(), ctx(), kCtxLen, box.ciphertext.data(), box.ciphertext.size(),
                       box.signature.data());
    });
    bench("bacap: next index", [&] { w.index.next(); });
    bench("bacap: advance an index by 64", [&] { w.index.advance_to(w.index.idx + 64); });

    // ---- the primitives under them ----
    uint8_t key[32], nonce[12] = {}, out[64], pk[32], sk[64];
    rnd(key, sizeof key);
    crypto_sign_keypair(pk, sk);
    auto gs = kq::gcmsiv_seal(key, nonce, nullptr, 0, padded.data(), padded.size());
    bench("aes-gcm-siv: seal 1553 B", [&] { kq::gcmsiv_seal(key, nonce, nullptr, 0, padded.data(), padded.size()); });
    bench("aes-gcm-siv: open 1553 B", [&] { Bytes pt; kq::gcmsiv_open(key, nonce, nullptr, 0, gs.data(), gs.size(), pt); });
    {
        // AES alone, as many blocks as a box: the rest of AES-GCM-SIV's time is POLYVAL.
        mbedtls_aes_context a;
        mbedtls_aes_init(&a);
        mbedtls_aes_setkey_enc(&a, key, 256);
        uint8_t blk[16] = {};
        size_t blocks = (padded.size() + 15) / 16;
        bench("aes-256: " + std::to_string(blocks) + " blocks (a box's keystream)", [&] {
            for (size_t i = 0; i < blocks; i++) mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_ENCRYPT, blk, blk);
        });
        mbedtls_aes_free(&a);
    }
    bench("ed25519: blind a public key", [&] { kq::ed_blind_public(pk, key, 32, out); });
    bench("ed25519: verify", [&] { kq::ed_verify(pk, padded.data(), padded.size(), box.signature.data()); });
    bench("hkdf-blake2b: 96 B", [&] {
        uint8_t o[96];
        kq::hkdf_blake2b(64, key, 32, nullptr, 0, ctx(), kCtxLen, o, sizeof o);
    });

    // ---- encodings ----
    kq1::Message m;
    m.content = kq1::text_content(Bytes(32, 1), std::string(200, 'x'), 4);
    m.acks = {{1, 10}, {2, 11}, {3, 12}};
    auto boxm = kq1::box_of(m, 4);
    bench("wire: pad a box", [&] { wire::pad(text.data(), text.size(), kBox); });
    bench("wire: write query", [&] { wire::write_query(box.box_id.data(), box.signature.data(), box.ciphertext.data(), box.ciphertext.size()); });
    auto q = wire::write_query(box.box_id.data(), box.signature.data(), box.ciphertext.data(), box.ciphertext.size());
    bench("wire: split a query into Channel frames", [&] { wire::split(1, q.data(), q.size(), wire::piece_size(500)); });
    bench("kq1: message to its box (v4)", [&] { kq1::box_of(m, 4); });
    if (!kq1::message_from_cbor(kq1::unframe(*boxm).data)) { fprintf(stderr, "bench: the box does not decode\n"); return 1; }
    bench("kq1: message from its box", [&] { kq1::message_from_cbor(kq1::unframe(*boxm).data); });

    // ---- vouchers: what joining costs ----
    Bytes mwc = w.bytes();
    auto minted = *kq1::voucher::mint(mwc, "kate", kq1::voucher::COMPACT, rnd);
    Bytes salt(32, 3), who(600, 4);
    auto ind = *kq1::voucher::induct(minted.voucher.data(), minted.payload, who, salt, rnd);
    bench("voucher: mint (compact)", [&] { kq1::voucher::mint(mwc, "kate", kq1::voucher::COMPACT, rnd); });
    bench("voucher: induct (compact)", [&] { kq1::voucher::induct(minted.voucher.data(), minted.payload, who, salt, rnd); });
    bench("voucher: open the reply (compact)", [&] { kq1::voucher::open_reply(minted.secret_key, minted.public_key, ind.sealed, mwc); });
    auto mintedL = *kq1::voucher::mint(mwc, "kate", kq1::voucher::LONG, rnd);
    auto indL = *kq1::voucher::induct(mintedL.voucher.data(), mintedL.payload, who, salt, rnd);
    bench("voucher: mint (long)", [&] { kq1::voucher::mint(mwc, "kate", kq1::voucher::LONG, rnd); });
    bench("voucher: induct (long)", [&] { kq1::voucher::induct(mintedL.voucher.data(), mintedL.payload, who, salt, rnd); });
    bench("voucher: open the reply (long)", [&] { kq1::voucher::open_reply(mintedL.secret_key, mintedL.public_key, indL.sealed, mwc); });
    bench("voucher: derive the voucher stream", [&] { kq1::voucher::derive_stream(minted.voucher.data()); });

    // ---- the state on flash: a group of 5 with 200 lines ----
    std::vector<Bytes> founders;
    for (int i = 0; i < 5; i++) founders.push_back(bacap::WriteCap::generate(rnd).read_cap().bytes());
    kq1::Chat c;
    c.id = c.name = "a group";
    c.write_cap = w.bytes();
    c.next_write = w.index.bytes();
    c.group = kq1::Group::found(founders[0], founders, kq1::Retention{}, 256);
    for (size_t i = 0; i < 200; i++) {
        kq1::Line l;
        l.id = c.fresh();
        l.sender = Bytes(32, (uint8_t)(i % 5));
        l.index = w.index.bytes();
        l.text = std::string(80, 'm');
        c.log.push_back(l);
    }
    kq1::State st{"tdeck", {c}};
    kq1::Changes step;  // a message sent: the record, and one line
    step.chats.insert(c.id);
    step.lines[c.id].insert(199);
    std::map<std::string, std::string> files;
    kq::Store plain(kq::memory_files(files));
    plain.save_all(st);
    bench("store: save after a message (plain)", [&] {
        st.chats[0].next_id++;  // so the record differs and is written
        plain.commit(st, step);
    });
    Bytes skey(32, 9);
    std::map<std::string, std::string> sfiles;
    kq::Store sealed(kq::sealed_files(kq::memory_files(sfiles), skey));
    sealed.save_all(st);
    bench("store: save after a message (sealed)", [&] {
        st.chats[0].next_id++;
        sealed.commit(st, step);
    });
    bench("store: load (plain)", [&] { kq::Store(kq::memory_files(files)).load("x"); });
    bench("store: load (sealed)", [&] { kq::Store(kq::sealed_files(kq::memory_files(sfiles), skey)).load("x"); });
    std::string rec;  // the largest record: one JSON document (a journal is one per line)
    for (auto& [n, d] : files)
        if (n.size() > 5 && n.substr(n.size() - 5) == ".json" && d.size() > rec.size()) rec = d;
    bench("json: parse the largest file (" + std::to_string(rec.size() / 1024) + " KiB)", [&] { kq::Json::parse(rec); });

    // ---- the passphrase ----
    bench("argon2id: unlock (4 MiB, 3 passes)", [] {
        uint8_t k[32], s[crypto_pwhash_SALTBYTES] = {};
        crypto_pwhash(k, 32, "correct horse", 13, s, 3, 4 << 20, crypto_pwhash_ALG_ARGON2ID13);
    });

    // ---- what a user does, as the operations it is made of ----
    struct Action {
        std::string what;
        std::vector<std::pair<std::string, double>> parts;  // operation, how many
    };
    double members = 4;  // the others whose streams are read
    std::vector<Action> actions = {
        {"join with a voucher (compact): the joiner",
         {{"voucher: mint (compact)", 1}, {"voucher: open the reply (compact)", 1}}},
        {"add a member with their voucher (compact)", {{"voucher: induct (compact)", 1}}},
        {"send a message", {{"kq1: message to its box (v4)", 1}, {"wire: pad a box", 1},
                            {"bacap: encrypt and sign a box", 1}, {"bacap: next index", 1},
                            {"store: save after a message (sealed)", 1}}},
        {"read a message that arrived", {{"bacap: box ID (blind the root key)", 1}, {"bacap: verify and decrypt a box", 1},
                                        {"kq1: message from its box", 1}, {"store: save after a message (sealed)", 1}}},
        {"poll every member for something new (4 others)", {{"bacap: box ID (blind the root key)", members}}},
        {"unlock after a start", {{"argon2id: unlock (4 MiB, 3 passes)", 1}, {"store: load (sealed)", 1}}},
    };

    std::sort(g_results.begin(), g_results.end(), [](auto& a, auto& b) { return a.us > b.us; });
    printf("## Operations, slowest first\n\n| operation | time | runs |\n|---|---:|---:|\n");
    for (auto& r : g_results) printf("| %s | %s | %zu |\n", r.name.c_str(), fmt(r.us).c_str(), r.reps);
    printf("\n## What a user does\n\n| action | time | made of |\n|---|---:|---|\n");
    for (auto& a : actions) {
        double t = 0;
        std::string parts;
        for (auto& [op, n] : a.parts) {
            t += us(op) * n;
            parts += (parts.empty() ? "" : ", ") + (n != 1 ? std::to_string((int)n) + "× " : std::string()) + op;
        }
        printf("| %s | %s | %s |\n", a.what.c_str(), fmt(t).c_str(), parts.c_str());
    }
    return 0;
}
