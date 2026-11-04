// CTIDH-1024 field helpers, the hybrid NIKE, sealed box and MKEM, against hpqc's vectors.
#include <sodium.h>

#include <random>

#include "kq_nike.h"
#include "vectors.h"

#define restrict __restrict
extern "C" {
#include "fiat_p1024.h"
}

using namespace kq;

int main(int, char** argv) {
    const char* path = argv[1];
    if (sodium_init() < 0) return 2;

#ifdef __SIZEOF_INT128__
    {
        // The helpers in use (with or without __int128) against 128-bit arithmetic.
        Vec v{"fiat", "helpers", {}};
        std::mt19937_64 r(1);
        const uint64_t edge[] = {0, 1, 2, 0x7fffffffffffffffULL, 0x8000000000000000ULL, 0xffffffffULL,
                                 0x100000000ULL, 0xfffffffffffffffeULL, 0xffffffffffffffffULL};
        int bad = 0;
        for (int i = 0; i < 200000; i++) {
            uint64_t a = i < 81 ? edge[i % 9] : r(), b = i < 81 ? edge[i / 9] : r();
            uint8_t c = i & 1;
            unsigned __int128 m = (unsigned __int128)a * b;
            uint64_t lo, hi;
            fiat_p1024_mulx_u64(&lo, &hi, a, b);
            bad += lo != (uint64_t)m || hi != (uint64_t)(m >> 64);
            unsigned __int128 s = (unsigned __int128)a + b + c;
            fiat_p1024_uint1 co;
            uint64_t o;
            fiat_p1024_addcarryx_u64(&o, &co, c, a, b);
            bad += o != (uint64_t)s || co != (uint8_t)(s >> 64);
            __int128 d = (__int128)a - b - c;
            fiat_p1024_subborrowx_u64(&o, &co, c, a, b);
            bad += o != (uint64_t)d || co != (d < 0);
        }
        check(bad == 0, v, "mulx/addcarryx/subborrowx");
    }
#endif

    for (auto& v : load(path, "nike/sealedbox.json")) {
        if (v.s("nike_name") == "x25519") continue;  // only the hybrid is used here
        auto priv = v.hex("recipient_private_key_hex"), pub = v.hex("recipient_public_key_hex"),
             ct = v.hex("ciphertext_hex");
        auto pt = nike::open(priv, pub, ct.data(), ct.size(), v.s("nike_name"));
        check(pt && *pt == v.hex("plaintext_hex"), v, "open");
        ct.back() ^= 1;
        check(!nike::open(priv, pub, ct.data(), ct.size(), v.s("nike_name")), v, "open tampered");
    }
    for (auto& v : load(path, "nike/mkem.json")) {
        auto ct = v.hex("ciphertext_hex");
        // recipient_private_keys_hex is a JSON list of hex strings.
        std::string list = v.s("recipient_private_keys_hex");
        for (size_t p = 0; (p = list.find('"', p)) != std::string::npos;) {
            size_t q = list.find('"', p + 1);
            Vec one{v.file, v.name, {{"k", list.substr(p + 1, q - p - 1)}}};
            auto pt = nike::mkem_decapsulate(one.hex("k"), ct.data(), ct.size());
            check(pt && *pt == v.hex("plaintext_hex"), v, "decapsulate");
            p = q + 1;
        }
    }

    // Voucher keys and seals: SHAKE256 seeds make CTIDH key generation deterministic.
    for (const char* f : {"voucher/voucher_vectors_v2.json", "voucher/voucher_vectors.json"}) {
        bool v2 = std::string(f).find("v2") != std::string::npos;
        auto vs = load(path, f);
        auto get = [&](const char* sect) -> const Vec& {
            for (auto& x : vs) if (x.name == sect) return x;
            fprintf(stderr, "no %s\n", sect);
            exit(2);
        };
        const Vec &in = get("inputs"), &mint = get("mint"), &induct = get("induct");
        auto seed = in.hex("keypair_seed");
        nike::Shake sh(seed.data(), seed.size());
        auto kp = nike::generate(sh.rng());
        check(kp.priv == mint.hex("voucher_secret_key"), mint, "seal secret key from seed");
        check(kp.pub == mint.hex("voucher_public_key"), mint, "seal public key from seed");
        check(nike::valid_public(kp.pub), mint, "public key validates");

        auto sealed = induct.hex("sealed_reply");
        auto pt = v2 ? nike::open(kp.priv, kp.pub, sealed.data(), sealed.size())
                     : nike::mkem_decapsulate(kp.priv, sealed.data(), sealed.size());
        check(pt.has_value(), induct, "open sealed reply");
        if (!pt) continue;
        auto sseed = in.hex("seal_seed");
        nike::Shake ss(sseed.data(), sseed.size());
        auto again = v2 ? nike::seal(kp.pub, pt->data(), pt->size(), ss.rng())
                        : nike::mkem_encapsulate(kp.pub, pt->data(), pt->size(), ss.rng());
        check(again == sealed, induct, "reseal from seal_seed");
        auto reply = kq1::Term::read(pt->data(), pt->size());
        check(reply.has_value(), induct, "reply is CBOR");
        if (!reply) continue;
        auto who = v2 ? (reply->kind == kq1::Term::LIST && reply->items.size() == 2 ? &reply->items[0] : nullptr)
                      : reply->field("WhoReply");
        check(who && who->bytes == in.hex("who_reply"), induct, "who_reply inside");
    }
    return finish("nike");
}
