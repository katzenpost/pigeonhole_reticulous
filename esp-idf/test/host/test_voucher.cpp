// SPDX-License-Identifier: AGPL-3.0-only
// Vouchers against hpqc voucher/testdata: mint, induct and open, in both forms.
#include <sodium.h>

#include "kq1_voucher.h"
#include "vectors.h"

using namespace kq;
namespace v1 = kq1::voucher;

int main(int, char** argv) {
    const char* path = argv[1];
    if (sodium_init() < 0) return 2;
    for (const char* f : {"voucher/voucher_vectors_v2.json", "voucher/voucher_vectors.json"}) {
        int form = std::string(f).find("v2") != std::string::npos ? v1::COMPACT : v1::LONG;
        auto vs = load(path, f);
        auto get = [&](const char* sect) -> const Vec& {
            for (auto& x : vs) if (x.name == sect) return x;
            exit(2);
        };
        const Vec &in = get("inputs"), &mint = get("mint"), &ind = get("induct"), &open = get("open");
        auto seed = in.hex("keypair_seed");
        nike::Shake ks(seed.data(), seed.size());
        auto m = v1::mint(in.hex("message_write_cap"), in.s("display_name"), form, ks.rng());
        check(m.has_value(), mint, "mints");
        if (!m) continue;
        check(Bytes(m->voucher.begin(), m->voucher.end()) == mint.hex("voucher"), mint, "voucher");
        check(m->payload == mint.hex("voucher_payload"), mint, "payload");
        check(m->stream.bytes() == mint.hex("voucher_write_cap"), mint, "voucher write cap");
        check(m->stream.read_cap().bytes() == mint.hex("voucher_read_cap"), mint, "voucher read cap");
        check(m->secret_key == mint.hex("voucher_secret_key") && m->public_key == mint.hex("voucher_public_key"), mint,
              "seal keypair");
        check(v1::payload_form(m->payload) == form, mint, "form from first byte");

        auto sseed = in.hex("seal_seed");
        nike::Shake ss(sseed.data(), sseed.size());
        auto r = v1::induct(m->voucher.data(), m->payload, in.hex("who_reply"), in.hex("salt"), ss.rng());
        check(r.has_value(), ind, "inducts");
        if (!r) continue;
        check(r->form == form && r->name == ind.s("display_name"), ind, "form and name");
        check(r->mutated_read_cap == ind.hex("mutated_message_read_cap"), ind, "live read cap");
        check(r->sealed == ind.hex("sealed_reply"), ind, "sealed reply");

        auto o = v1::open_reply(m->secret_key, m->public_key, r->sealed, in.hex("message_write_cap"));
        check(o.has_value(), open, "opens");
        if (!o) continue;
        check(o->who_reply == open.hex("who_reply") && o->salt == open.hex("salt"), open, "who reply and salt");
        check(o->mutated_write_cap == open.hex("mutated_message_write_cap"), open, "live write cap");

        auto bad = m->payload;
        bad.back() ^= 1;
        nike::Shake ss2(sseed.data(), sseed.size());
        check(!v1::induct(m->voucher.data(), bad, in.hex("who_reply"), in.hex("salt"), ss2.rng()), ind, "tampered payload");

        auto boxes = v1::reply_boxes(r->sealed, form, 100);
        Bytes joined;
        bool framed = true;
        for (size_t i = 0; i < boxes.size(); i++) {
            framed &= form == v1::LONG || (boxes[i].size() <= 100 && boxes[i][0] == (i + 1 < boxes.size() ? 'C' : 'F'));
            joined.insert(joined.end(), boxes[i].begin() + (form == v1::LONG ? 0 : 1), boxes[i].end());
        }
        check(framed && joined == r->sealed && (form == v1::COMPACT || boxes.size() == 1), ind, "reply boxes");
    }
    return finish("voucher");
}
