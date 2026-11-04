// SPDX-License-Identifier: AGPL-3.0-only
// Pigeonhole queries, replies and padding against pigeonhole_rns's bridge vectors, and the
// Channel frames as pigeonhole_rns tests/test_channel.py exercises them.
#include <sodium.h>

#include <algorithm>
#include <random>

#include "kq_bacap.h"
#include "kq_wire.h"
#include "vectors.h"

using namespace kq;
using namespace kq::wire;

static Bytes rnd(size_t n) { Bytes b(n); if (n) randombytes_buf(b.data(), n); return b; }

static std::vector<Bytes> feed_all(Reassembler& r, const std::vector<Frame>& fs, uint32_t now = 0) {
    std::vector<Bytes> out;
    for (auto& f : fs) {
        auto packed = f.pack();
        auto back = Frame::unpack(packed.data(), packed.size());
        if (auto d = r.feed(*back, now)) out.push_back(*d);
    }
    return out;
}

int main(int, char** argv) {
    const char* path = argv[1];
    if (sodium_init() < 0) return 2;

    for (auto& v : load(path, "pigeonhole/bridge_socket_vectors.json")) {
        if (v.name.rfind("queries/", 0) == 0) {
            auto id = v.hex("box_id");
            Bytes q = v.s("kind") == "0" ? read_query(id.data())
                                         : write_query(id.data(), v.hex("signature").data(), v.hex("ciphertext").data(),
                                                       v.hex("ciphertext").size());
            check(q == v.hex("payload"), v, "query bytes");
        } else if (v.name.rfind("replies/", 0) == 0) {
            auto p = v.hex("payload");
            auto r = decode_reply(p.data(), p.size());
            check(r && r->code == std::stoi(v.s("code")), v, "reply code");
            if (r && v.has("signature")) {
                check(r->found && Bytes(r->signature.begin(), r->signature.end()) == v.hex("signature"), v, "signature");
                check(r->ciphertext == v.hex("ciphertext"), v, "ciphertext");
            }
        } else if (v.name.rfind("boxes/", 0) == 0) {
            // What a client reading the box concludes: it opens only the boxes a replica stores.
            auto id = v.hex("box_id"), sig = v.hex("signature"), ct = v.hex("ciphertext");
            bool ok = sig.size() == 64 && ed_verify(id.data(), ct.data(), ct.size(), sig.data());
            int expect = std::stoi(v.s("expect"));
            check(ok == (expect == 0) || expect == INVALID_PAYLOAD, v, "verify as the replica does");
        }
    }

    Vec v{"channel", "frames", {}};
    const uint8_t layout[] = {1, 2, 3, 9, 'a', 'b', 'c'};
    Frame f{0x0102, 3, 9, Bytes{'a', 'b', 'c'}};
    check(f.pack() == Bytes(layout, layout + sizeof layout), v, "frame layout");
    check(piece_size(500) == 421, v, "piece size at MTU 500");
    const size_t P = 421;
    for (size_t n : {0ul, 1ul, 33ul, P - 1, P, P + 1, 1000ul, 3 * P, kMaxPieces * P}) {
        auto data = rnd(n);
        auto fs = split(7, data.data(), n, P);
        check(fs.size() == std::max<size_t>(1, (n + P - 1) / P), v, "piece count");
        Reassembler r;
        auto got = feed_all(r, fs);
        check(got.size() == 1 && got[0] == data, v, "split/reassemble");
    }
    auto big = rnd(kMaxPieces * P + 1);
    check(split(1, big.data(), big.size(), P).empty(), v, "256 pieces refused");
    check(write_query(rnd(32).data(), rnd(64).data(), rnd(1020).data(), 1020).size() == 1117, v, "write of B=1000");
    {
        auto a = rnd(2000), b = rnd(1500);
        auto fa = split(1, a.data(), a.size(), P), fb = split(2, b.data(), b.size(), P);
        std::vector<Frame> fs = fa;
        fs.insert(fs.end(), fb.begin(), fb.end());
        fs.insert(fs.end(), fa.begin(), fa.begin() + 2);
        fs.insert(fs.end(), fb.begin() + 1, fb.begin() + 3);
        std::shuffle(fs.begin(), fs.end(), std::mt19937(4));
        Reassembler r;
        auto got = feed_all(r, fs);
        std::sort(got.begin(), got.end());
        std::vector<Bytes> want{a, b};
        std::sort(want.begin(), want.end());
        check(got == want, v, "interleaved, out of order, duplicated");
    }
    {
        Reassembler r;
        auto d = rnd(1000);
        auto fs = split(3, d.data(), d.size(), P);
        check(!r.feed(fs[0], 0), v, "first piece");
        check(!r.feed(Frame{3, 1, 4, fs[1].data}, 0) && r.pending() == 0, v, "count mismatch drops");
        check(!r.feed(fs[2], 0) && r.pending() == 1, v, "fresh incomplete request");
        check(!r.feed(Frame{4, 2, 2, {}}, 0) && !r.feed(Frame{4, 0, 0, {}}, 0), v, "bad index/count");
    }
    {
        Reassembler r(10000);
        auto d = rnd(1000);
        auto fs = split(9, d.data(), d.size(), P);
        r.feed(fs[0], 0);
        r.feed(fs[1], 0);
        check(!r.feed(fs[2], 11000) && r.pending() == 1, v, "incomplete timed out");
    }
    {
        // Padding and a whole box: pad, BACAP-encrypt, query, reply, verify, open, unpad.
        auto msg = rnd(300);
        auto padded = *pad(msg.data(), msg.size(), 1000);
        check(padded.size() == 1004 && *unpad(padded.data(), padded.size()) == msg, v, "pad/unpad");
        check(!pad(msg.data(), 1001, 1000), v, "oversize refused");
        uint8_t bad[8] = {0, 0, 0, 9};
        check(!unpad(bad, sizeof bad), v, "length past the end refused");
        uint8_t seed = 1;
        auto w = bacap::WriteCap::generate([&](uint8_t* p, size_t n) { while (n--) *p++ = seed++; });
        auto ctx = (const uint8_t*)bacap::kPigeonholeCtx;
        size_t ctx_len = sizeof bacap::kPigeonholeCtx - 1;
        auto s = bacap::encrypt(w, w.index, ctx, ctx_len, padded.data(), padded.size());
        check(s.ciphertext.size() == 1020, v, "ciphertext is B + 20");
        auto q = write_query(s.box_id.data(), s.signature.data(), s.ciphertext.data(), s.ciphertext.size());
        Bytes rep{SUCCESS};
        rep.insert(rep.end(), q.begin() + 33, q.end());
        auto r = decode_reply(rep.data(), rep.size());
        auto id = bacap::box_id(w.read_cap().root_pk, w.index, ctx, ctx_len);
        auto pt = bacap::decrypt(w.index, id->data(), ctx, ctx_len, r->ciphertext.data(), r->ciphertext.size(),
                                 r->signature.data());
        check(pt && *unpad(pt->data(), pt->size()) == msg, v, "box round trip");
    }
    return finish("wire");
}
