// SPDX-License-Identifier: AGPL-3.0-only
// The katzenqt1 wire against pigeonhole_protocols testdata/katzenqt1, as its Rust tests
// (tests/katzenqt1_wire_vectors*.rs) read them.
#include <sodium.h>

#include <algorithm>

#include "kq1_wire.h"
#include "vectors.h"

using namespace kq1;

static Json opt_hex(const std::optional<Bytes>& b) { return b ? Json::str(tohex(*b)) : Json{}; }

static Json message_json(const std::optional<Message>& m) {
    if (!m) return Json{};
    std::vector<Json> numbers, acks;
    for (auto& n : m->numbers) numbers.push_back(Json::arr({Json::num(n.index), opt_hex(n.prefix)}));
    for (auto& a : m->acks) acks.push_back(Json::arr({Json::num(a.member), Json::num(a.idx64)}));
    Json intro, at;
    if (m->introduction)
        intro = Json::obj({{"name", Json::str(m->introduction->name)}, {"read_cap", Json::str(tohex(m->introduction->read_cap))}});
    if (m->introduced_at)
        at = Json::obj({{"introducer", Json::num(m->introduced_at->introducer)}, {"index", Json::str(tohex(m->introduced_at->index))}});
    return Json::obj({{"content", opt_hex(m->content)}, {"introduction", intro}, {"numbers", Json::arr(numbers)},
                      {"introduced_at", at}, {"acks", Json::arr(acks)}, {"membership_hash", opt_hex(m->membership_hash)}});
}

static Json join_reply_json(const std::optional<JoinReply>& r) {
    if (!r) return Json{};
    std::vector<Json> members, rosters, intros;
    for (auto& m : r->members)
        members.push_back(m ? Json::arr({Json::str(tohex(m->read_cap)), Json::str(tohex(m->start))}) : Json{});
    for (auto& ro : r->rosters) {
        if (!ro) { rosters.push_back(Json{}); continue; }
        std::vector<Json> es;
        for (auto& e : *ro) es.push_back(Json::arr({Json::num(e.index), e.place ? Json::num(*e.place) : Json{}}));
        rosters.push_back(Json::arr(es));
    }
    for (auto& i : r->introductions)
        intros.push_back(Json::arr({Json::num(i.by), Json::str(tohex(i.index)), Json::num(i.member)}));
    return Json::obj({{"members", Json::arr(members)}, {"rosters", Json::arr(rosters)}, {"introductions", Json::arr(intros)},
                      {"introducer", Json::num(r->introducer)}, {"newcomer", Json::num(r->newcomer)}});
}

static Message read_message(const Json& j) {
    Message m;
    if (!j["content"].is_null()) m.content = unhex(j["content"].s);
    if (!j["introduction"].is_null())
        m.introduction = Introduction{j["introduction"]["name"].s, unhex(j["introduction"]["read_cap"].s)};
    for (auto& p : j["numbers"].a)
        m.numbers.push_back({(size_t)p.a[0].u(), p.a[1].is_null() ? std::nullopt : std::optional<Bytes>(unhex(p.a[1].s))});
    if (!j["introduced_at"].is_null())
        m.introduced_at = IntroRef{(size_t)j["introduced_at"]["introducer"].u(), unhex(j["introduced_at"]["index"].s)};
    for (auto& p : j["acks"].a) m.acks.push_back({(size_t)p.a[0].u(), p.a[1].u()});
    if (!j["membership_hash"].is_null()) m.membership_hash = unhex(j["membership_hash"].s);
    return m;
}

static std::optional<Bytes> map_gcm(const Message& m) {
    auto g = message_to_gcm(m, MAP_VERSION);
    if (!g) return std::nullopt;
    g->version = std::min(g->version, MAP_VERSION);
    return g->to_bytes();
}

static std::vector<Ack> read_acks(const Json& j) {
    std::vector<Ack> acks;
    for (auto& p : j.a) acks.push_back({(size_t)p.a[0].u(), p.a[1].u()});
    std::stable_sort(acks.begin(), acks.end(), [](const Ack& x, const Ack& y) { return x.member < y.member; });
    return acks;
}

int main(int, char** argv) {
    const char* path = argv[1];
    if (sodium_init() < 0) return 2;

    for (bool v4 : {true, false}) {
        std::string f = v4 ? "katzenqt1/groupchat_wire_vectors_v4.json" : "katzenqt1/groupchat_wire_vectors.json";
        for (auto& v : load(path, f)) {
            const std::string& sect = v.s("section");
            if (sect == "messages") {
                auto cbor = v.hex("cbor");
                auto got = message_from_cbor(cbor);
                check(message_json(got) == Json::parse(v.s("message")), v, "read");
                if (!got) {
                    check(!v4 && (!v.has("rewritten") || v.s("rewritten") == "null"), v, "refused");
                    continue;
                }
                auto rw = v.hex("rewritten");
                auto back = v4 ? message_to_gcm(*got, CURRENT_VERSION).value().to_bytes() : *map_gcm(*got);
                check(back == rw, v, "written back");
                if (rw == cbor) check(Gcm::from_bytes(cbor)->to_bytes() == cbor, v, "encodes to itself");
            } else if (sect == "written") {
                auto m = read_message(Json::parse(v.s("message")));
                auto b = v4 ? box_of(m, CURRENT_VERSION) : std::optional<Bytes>(frame(*map_gcm(m)));
                check(b && *b == v.hex("box"), v, "box");
            } else if (sect == "membership") {
                std::vector<Bytes> caps;
                for (auto& c : Json::parse(v.s("read_caps")).a) caps.push_back(unhex(c.s));
                auto h = membership_hash(caps);
                check(Bytes(h.begin(), h.end()) == v.hex("hash"), v, "membership hash");
            } else if (sect == "replies") {
                auto cbor = v.hex("cbor");
                auto w = ReplyWho::from_bytes(cbor);
                check(w.has_value(), v, "decodes");
                if (!w) continue;
                check(join_reply_json(w->to_join_reply()) == Json::parse(v.s("join_reply")), v, "join reply");
                if (v4) {
                    auto again = w->to_bytes();
                    check(again[0] >> 5 == 4 && ReplyWho::from_bytes(again)->to_bytes() == again, v, "array round trip");
                } else {
                    check(w->to_map_bytes() == cbor, v, "map encoding");
                }
            } else if (sect == "unsupported") {
                auto u = unsupported(v.hex("cbor"));
                std::string want = v.s("version") == "null" ? "null" : v.s("version") + "," + v.s("msg_type");
                check((u ? std::to_string(u->first) + "," + std::to_string(u->second) : "null") == want, v, "unsupported");
            } else if (sect == "malformed") {
                auto cbor = v.hex("cbor");
                check(!Gcm::from_bytes(cbor) && !message_from_cbor(cbor), v, "refused");
            }
        }
    }
    for (auto& v : load(path, "katzenqt1/acks_vectors.json")) {
        const std::string& sect = v.s("section");
        auto field = v.hex("field");
        if (sect == "valid") {
            auto acks = read_acks(Json::parse(v.s("acks")));
            check(acks_to_bytes(acks) == field, v, "acks to bytes");
            check(acks_from_bytes(field) == acks, v, "acks from bytes");
        } else if (sect == "lenient") {
            check(acks_from_bytes(field) == read_acks(Json::parse(v.s("acks"))), v, "lenient");
        } else {
            check(!acks_from_bytes(field), v, "malformed acks refused");
        }
    }

    // Files as pigeonhole_protocols writes them (vectors/katzenqt1/files.rs): written alike here,
    // and read back from its boxes.
    for (auto& v : load(path, "katzenqt1/files.json")) {
        Json j = Json::parse(v.s("chunks"));
        std::vector<Bytes> chunks;
        for (auto& c : j.a) chunks.push_back(unhex(c.s));
        auto payload = v.hex("payload"), message = v.hex("message"), read_cap = v.hex("read_cap");
        size_t max = v.u("box_size");
        FileUpload f{{}, v.s("filetype"), v.s("basename")};
        auto content = file_content(v.hex("membership_hash"), f, v.u("version"));
        check(content == v.hex("content") && filetype_of(f.basename) == f.filetype, v, "the message, written alike");
        check(put_payload(content, payload) == message && frame(message) == v.hex("box"), v, "its payload put in");
        check(v.b("fits") == (message.size() + 1 <= max) && chunk_count(message.size(), max) == chunks.size(), v,
              "as many chunks");
        bool same = true;
        for (size_t i = 0; i < chunks.size(); i++) same &= substream_box(message, max, i) == chunks[i];
        check(same && indirection(chunks.size(), read_cap) == v.hex("indirection"), v, "the chunks and the I box, alike");
        auto ch = unframe(v.hex("indirection"));
        check(ch.kind == Chunk::INDIRECTION && ch.total == chunks.size() && ch.data == read_cap, v, "the I box read");
        check(assemble(chunks) == message, v, "its chunks assembled");
        auto t = take_payload(message);
        auto g = Gcm::from_bytes(message);
        auto file = g ? file_of(*g) : std::nullopt;
        check(t && t->first == content && t->second == payload && message_from_cbor(t->first), v, "its payload taken out");
        check(file && file->payload == payload && file->filetype == f.filetype && file->basename == f.basename, v,
              "the file read");
    }

    Vec v{"kq1", "unit", {}};
    Bytes H(HASH_SIZE, 0);
    auto text = text_content(H, "hello", CURRENT_VERSION);
    check(text[0] >> 5 == 4 && text_of(text) == "hello", v, "v4 text");
    Gcm m3;
    m3.version = MAP_VERSION;
    m3.membership_hash = H;
    m3.msg_type = MSG_TEXT;
    m3.text = "hello";
    auto b3 = m3.to_bytes();
    check(b3[0] >> 5 == 5 && text_of(b3) == "hello" && Gcm::from_bytes(b3)->to_bytes() == b3, v, "v3 text");
    check(text_content(H, "", CURRENT_VERSION).size() == 38, v, "empty text costs 38");
    check(text_content(H, std::string(24, 'a'), CURRENT_VERSION).size() == 39 + 24, v, "text header grows");
    Gcm intro;
    intro.version = CURRENT_VERSION;
    intro.membership_hash = H;
    intro.msg_type = MSG_INTRODUCTION;
    intro.introduction = Introduction{"bob", Bytes(READ_CAP_SIZE, 1)};
    check(intro.to_bytes().size() == 180, v, "introduction costs 180");
    for (auto ns : std::vector<std::vector<Number>>{{}, {{7, std::nullopt}}, {{3, Bytes(8, 1)}, {4, Bytes(8, 2)}}})
        check(numbers_from_bytes(numbers_to_bytes(ns)) == ns, v, "numbers round trip");
    Bytes repeated{0};
    repeated.insert(repeated.end(), 16, 1);
    check(numbers_from_bytes(repeated).empty(), v, "repeated prefix refused");
    IntroRef r{9, Bytes(INDEX_SIZE, 5)};
    check(introduced_at_from_bytes(introduced_at_to_bytes(r)) == r, v, "introduced_at round trip");
    check(unframe(Bytes{'I', 0, 0, 0, 3}).kind == Chunk::UNKNOWN, v, "short I box");
    check(safe_name("../.x/é\x01.opus") == "_.x___.opus" && safe_name("...") == "unnamed" &&
              safe_name(std::string(300, 'a')).size() == 200, v, "safe names");
    check(std::string(filetype_of("A.OPUS")) == "audio/opus" && std::string(filetype_of("x.")) == "arbitrary" &&
              std::string(filetype_of("noext")) == "arbitrary", v, "file types");
    check(chunk_count(1548, 1549) == 1 && chunk_count(1549, 1549) == 2 && chunk_count(3096, 1549) == 2 &&
              chunk_count(3097, 1549) == 3, v, "chunk counts at the edges");
    check(!take_payload(text) && !put_payload(text, Bytes{1}), v, "a text has no payload");

    // A ping, as the vectors' "ping": msg_type 11, a null body.
    Bytes pc = unhex("840458204592987f08d443913f3f0d0cc916a79db26d468c256676536cb19ce851a593c90bf6");
    check(ping_content(Bytes(pc.begin() + 4, pc.begin() + 36), CURRENT_VERSION) == pc &&
              Gcm::from_bytes(pc)->msg_type == MSG_PING, v, "a ping");
    return finish("kq1");
}
