// SPDX-License-Identifier: AGPL-3.0-only
// The JSON of member/trace.rs: what katzenqt1's member traces record, and the group as the
// client saves it.
#include "kq1_trace.h"

namespace kq1::trace {

static std::string tohex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}
static Bytes unhex(const std::string& h) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw Bad("not hex");
    };
    if (h.size() % 2) throw Bad("odd hex");
    Bytes b;
    for (size_t i = 0; i < h.size(); i += 2) b.push_back((uint8_t)(nib(h[i]) << 4 | nib(h[i + 1])));
    return b;
}

Json h(const Bytes& b) { return Json::str(tohex(b)); }
Json oh(const std::optional<Bytes>& b) { return b ? h(*b) : Json{}; }
Json nu(uint64_t x) { return Json::num(x); }
Json on(const std::optional<size_t>& x) { return x ? nu(*x) : Json{}; }


Json message(const Message& m) {
    std::vector<Json> ns, as;
    for (auto& x : m.numbers) ns.push_back(Json::arr({nu(x.index), oh(x.prefix)}));
    for (auto& a : m.acks) as.push_back(Json::arr({nu(a.member), nu(a.idx64)}));
    Json intro, at;
    if (m.introduction) intro = Json::obj({{"name", Json::str(m.introduction->name)}, {"read_cap", h(m.introduction->read_cap)}});
    if (m.introduced_at) at = Json::arr({nu(m.introduced_at->introducer), h(m.introduced_at->index)});
    return Json::obj({{"content", oh(m.content)}, {"introduction", intro}, {"numbers", Json::arr(ns)},
                      {"introduced_at", at}, {"acks", Json::arr(as)}, {"membership_hash", oh(m.membership_hash)}});
}
Json payload(const Payload& p) { return p.is_bytes ? Json::obj({{"bytes", h(p.bytes)}}) : message(p.message); }

Json join_reply(const JoinReply& r) {
    std::vector<Json> ms, rs, is;
    for (auto& m : r.members) ms.push_back(m ? Json::arr({h(m->read_cap), h(m->start)}) : Json{});
    for (auto& ro : r.rosters) {
        if (!ro) { rs.push_back(Json{}); continue; }
        std::vector<Json> es;
        for (auto& e : *ro) es.push_back(Json::arr({nu(e.index), on(e.place)}));
        rs.push_back(Json::arr(es));
    }
    for (auto& i : r.introductions) is.push_back(Json::arr({nu(i.by), h(i.index), nu(i.member)}));
    return Json::obj({{"type", Json::str("JoinReply")}, {"members", Json::arr(ms)}, {"rosters", Json::arr(rs)},
                      {"introductions", Json::arr(is)}, {"introducer", nu(r.introducer)}, {"newcomer", nu(r.newcomer)}});
}


Json at_j(const At& a) { return Json::arr({h(a.introducer), h(a.index)}); }
const char* mode_s(Mode m) { return m == Mode::IDLE ? "idle" : m == Mode::READING ? "reading" : "scanning"; }
const char* induction_s(Induction i) {
    return i == Induction::IDLE ? "idle" : i == Induction::CATCHING_UP ? "catching up" : "inducting";
}
Json reader(const Reader& r) {
    std::vector<Json> kept, tried;
    for (auto& k : r.kept) kept.push_back(Json::arr({h(k.index), nu(k.passed), oh(k.before)}));
    for (auto t : r.tried) tried.push_back(nu(t));
    return Json::obj({{"stream", h(r.stream)}, {"read_cap", h(r.read_cap)}, {"next", h(r.next)}, {"last", oh(r.last)},
                      {"passed", nu(r.passed)}, {"kept", Json::arr(kept)},
                      {"lost", r.lost ? Json::arr({nu(r.lost->passed), oh(r.lost->before)}) : Json{}},
                      {"mode", Json::str(mode_s(r.mode))}, {"tried", Json::arr(tried)}});
}
Json retention(const Retention& p) {
    return Json::obj({{"window", on(p.window)}, {"discard_acked", Json::boolean(p.discard_acked)},
                      {"keep_sent", on(p.keep_sent)}, {"keep_received", on(p.keep_received)}});
}
Json hexes(const std::vector<Bytes>& v) {
    std::vector<Json> a;
    for (auto& x : v) a.push_back(h(x));
    return Json::arr(a);
}
Json group_j(const Group& g) {
    auto& r = g.rosters;
    auto& l = g.ledger;
    auto key = [](const Key& k) { return h(k); };
    auto num = [](uint64_t x) { return nu(x); };
    Json rosters = Json::obj(
        {{"entries", pairs(r.entries, key,
                           [&](const RosterMap& ro) {
                               return pairs(ro, num, [](const Entry& e) {
                                   return Json::obj({{"key", oh(e.key)}, {"prefix", oh(e.prefix)}});
                               });
                           })},
         {"started", pairs(r.started, key, [](const Started& s) { return Json::arr({h(s.introducer), nu(s.top)}); })},
         {"introductions", pairs(r.introductions, at_j, key)},
         {"keys", hexes(r.keys)}});
    Json sent = Json::obj(
        {{"records", pairs(g.sent.records, num,
                           [](const Record& x) {
                               return Json::obj({{"index", h(x.index)}, {"plaintext", x.plaintext ? payload(*x.plaintext) : Json{}},
                                                 {"control", Json::boolean(x.control)}, {"written", nu(x.written)},
                                                 {"settled", Json::boolean(x.settled)}});
                           })},
         {"backfilled", g.sent.backfilled ? nu(*g.sent.backfilled) : Json::neg(-1)}});
    Json ledger = Json::obj(
        {{"published", pairs(l.published, key, key)},
         {"acked_by", pairs(l.acked_by, key, num)},
         {"heard", pairs(l.heard, key, [&](const OMap<Key, uint64_t>& m) { return pairs(m, key, num); })},
         {"induction", Json::str(induction_s(l.induction))},
         {"invitee", l.invitee ? Json::arr({Json::str(l.invitee->name), h(l.invitee->read_cap)}) : Json{}},
         {"commit", Json::boolean(l.commit)},
         {"starts", pairs(l.starts, key, key)}});
    std::vector<Json> unnumbered;
    for (auto& a : g.unnumbered) unnumbered.push_back(at_j(a));
    return Json::obj(
        {{"read_cap", h(g.read_cap)}, {"policy", retention(g.policy)}, {"recheck", nu(g.recheck)}, {"rosters", rosters},
         {"readers", pairs(g.readers, key, reader)}, {"sent", sent}, {"ledger", ledger}, {"epoch", nu(g.epoch)},
         {"pool", hexes(g.pool)}, {"tip", h(g.tip)}, {"deriving", Json::boolean(g.deriving)}, {"outbox", hexes(g.outbox)},
         {"pending", pairs(g.pending, num,
                           [&](const Pending& p) {
                               std::vector<Json> ns;
                               for (auto& x : p.numbers) ns.push_back(Json::arr({nu(x.index), x.at ? at_j(*x.at) : Json{}}));
                               return Json::obj({{"numbers", Json::arr(ns)}, {"introduced", oh(p.introduced)},
                                                 {"acks", pairs(p.acks, key,
                                                                [](const AckedRun& a) { return Json::arr({oh(a.before), h(a.now)}); })},
                                                 {"control", Json::boolean(p.control)}});
                           })},
         {"tokens", nu(g.tokens)}, {"unnumbered", Json::arr(unnumbered)},
         {"introduced_at", g.introduced_at ? Json::obj({{"introducer", nu(g.introduced_at->introducer)}, {"index", h(g.introduced_at->index)}})
                                           : Json{}},
         {"joined_through", oh(g.joined_through)}, {"awaiting_start", pairs(g.awaiting_start, key, key)},
         {"allocates", Json::boolean(g.allocates)},
         {"received", pairs(g.received, [](const ReceivedAt& x) { return Json::arr({h(x.sender), h(x.index)}); }, num)}});
}


const Json& get(const Json& j, const char* k) {
    auto it = j.o.find(k);
    if (j.kind != Json::OBJ || it == j.o.end()) throw Bad(std::string("no field ") + k);
    return it->second;
}
Bytes b(const Json& j) {
    if (j.kind != Json::STR) throw Bad("not a string: " + j.dump());
    return unhex(j.s);
}
std::optional<Bytes> ob(const Json& j) { return j.is_null() ? std::nullopt : std::optional<Bytes>(b(j)); }
uint64_t u(const Json& j) {
    if (j.kind != Json::NUM || j.s.empty() || j.s[0] == '-') throw Bad("not a natural number: " + j.dump());
    return j.u();
}
std::optional<size_t> ous(const Json& j) { return j.is_null() ? std::nullopt : std::optional<size_t>(u(j)); }
const std::vector<Json>& arr(const Json& j) {
    if (j.kind != Json::ARR) throw Bad("not a list: " + j.dump());
    return j.a;
}
const std::vector<Json>& tup(const Json& j, size_t n) {
    auto& a = arr(j);
    if (a.size() != n) throw Bad("not " + std::to_string(n) + " items: " + j.dump());
    return a;
}
bool bo(const Json& j) {
    if (j.kind != Json::BOOL) throw Bad("not a bool: " + j.dump());
    return j.b();
}
std::vector<Bytes> indexes_r(const Json& j) {
    std::vector<Bytes> v;
    for (auto& x : arr(j)) v.push_back(b(x));
    return v;
}

Message read_message(const Json& j) {
    Message m;
    m.content = ob(get(j, "content"));
    auto& intro = get(j, "introduction");
    if (!intro.is_null()) m.introduction = Introduction{get(intro, "name").s, b(get(intro, "read_cap"))};
    for (auto& x : arr(get(j, "numbers"))) {
        auto& p = tup(x, 2);
        m.numbers.push_back({(size_t)u(p[0]), ob(p[1])});
    }
    auto& ia = get(j, "introduced_at");
    if (!ia.is_null()) {
        auto& p = tup(ia, 2);
        m.introduced_at = IntroRef{(size_t)u(p[0]), b(p[1])};
    }
    for (auto& x : arr(get(j, "acks"))) {
        auto& p = tup(x, 2);
        m.acks.push_back({(size_t)u(p[0]), u(p[1])});
    }
    m.membership_hash = ob(j["membership_hash"]);
    return m;
}
std::optional<Message> read_opt_message(const Json& j) {
    return j.is_null() ? std::nullopt : std::optional<Message>(read_message(j));
}
Payload read_payload(const Json& j) {
    if (j.o.count("bytes")) return Payload::raw(b(j["bytes"]));
    return Payload::of(read_message(j));
}
JoinReply read_join_reply(const Json& j) {
    JoinReply r;
    for (auto& m : arr(get(j, "members"))) {
        if (m.is_null()) { r.members.push_back(std::nullopt); continue; }
        auto& p = tup(m, 2);
        r.members.push_back(Member{b(p[0]), b(p[1])});
    }
    for (auto& ro : arr(get(j, "rosters"))) {
        if (ro.is_null()) { r.rosters.push_back(std::nullopt); continue; }
        Roster es;
        for (auto& e : arr(ro)) {
            auto& p = tup(e, 2);
            es.push_back({(size_t)u(p[0]), ous(p[1])});
        }
        r.rosters.push_back(es);
    }
    for (auto& i : arr(get(j, "introductions"))) {
        auto& p = tup(i, 3);
        r.introductions.push_back({(size_t)u(p[0]), b(p[1]), (size_t)u(p[2])});
    }
    r.introducer = u(get(j, "introducer"));
    r.newcomer = u(get(j, "newcomer"));
    return r;
}
Retention read_retention(const Json& j) {
    Retention r;
    r.window = ous(get(j, "window"));
    r.discard_acked = bo(get(j, "discard_acked"));
    r.keep_sent = ous(get(j, "keep_sent"));
    r.keep_received = ous(get(j, "keep_received"));
    return r;
}


At read_at(const Json& j) {
    auto& p = tup(j, 2);
    return At{b(p[0]), b(p[1])};
}
Reader read_reader(const Json& j) {
    Reader r;
    r.stream = b(get(j, "stream"));
    r.read_cap = b(get(j, "read_cap"));
    r.next = b(get(j, "next"));
    r.last = ob(get(j, "last"));
    r.passed = u(get(j, "passed"));
    for (auto& k : arr(get(j, "kept"))) {
        auto& p = tup(k, 3);
        r.kept.push_back(Kept{b(p[0]), (size_t)u(p[1]), ob(p[2])});
    }
    auto& l = get(j, "lost");
    if (!l.is_null()) {
        auto& p = tup(l, 2);
        r.lost = Lost{(size_t)u(p[0]), ob(p[1])};
    }
    auto& m = get(j, "mode").s;
    r.mode = m == "reading" ? Mode::READING : m == "scanning" ? Mode::SCANNING : Mode::IDLE;
    for (auto& t : arr(get(j, "tried"))) r.tried.push_back(u(t));
    return r;
}
OMap<Key, Index> index_pairs(const Json& j) {
    OMap<Key, Index> m;
    each_pair(j, [&](const Json& k, const Json& v) { m.insert(b(k), b(v)); });
    return m;
}
OMap<Key, uint64_t> nat_pairs(const Json& j) {
    OMap<Key, uint64_t> m;
    each_pair(j, [&](const Json& k, const Json& v) { m.insert(b(k), u(v)); });
    return m;
}
Group read_group(const Json& j) {
    Group g;
    auto& r = get(j, "rosters");
    each_pair(get(r, "entries"), [&](const Json& k, const Json& v) {
        RosterMap ro;
        each_pair(v, [&](const Json& i, const Json& e) { ro.insert(u(i), Entry{ob(get(e, "key")), ob(get(e, "prefix"))}); });
        g.rosters.entries.insert(b(k), ro);
    });
    each_pair(get(r, "started"), [&](const Json& k, const Json& v) {
        auto& p = tup(v, 2);
        g.rosters.started.insert(b(k), Started{b(p[0]), (size_t)u(p[1])});
    });
    each_pair(get(r, "introductions"), [&](const Json& k, const Json& v) { g.rosters.introductions.insert(read_at(k), b(v)); });
    g.rosters.keys = indexes_r(get(r, "keys"));
    auto& sb = get(j, "sent");
    each_pair(get(sb, "records"), [&](const Json& k, const Json& v) {
        auto& p = get(v, "plaintext");
        Record rec{b(get(v, "index")), p.is_null() ? std::nullopt : std::optional<Payload>(read_payload(p)),
                   bo(get(v, "control")), (size_t)u(get(v, "written")), bo(get(v, "settled"))};
        g.sent.records.insert(u(k), rec);
    });
    auto& bf = get(sb, "backfilled");
    g.sent.backfilled = bf.s == "-1" ? std::nullopt : std::optional<size_t>(u(bf));
    auto& l = get(j, "ledger");
    each_pair(get(l, "heard"), [&](const Json& k, const Json& v) { g.ledger.heard.insert(b(k), nat_pairs(v)); });
    g.ledger.published = index_pairs(get(l, "published"));
    g.ledger.acked_by = nat_pairs(get(l, "acked_by"));
    auto& ind = get(l, "induction").s;
    g.ledger.induction = ind == "catching up" ? Induction::CATCHING_UP : ind == "inducting" ? Induction::INDUCTING : Induction::IDLE;
    auto& inv = get(l, "invitee");
    if (!inv.is_null()) {
        auto& p = tup(inv, 2);
        g.ledger.invitee = Invitee{p[0].s, b(p[1])};
    }
    g.ledger.commit = bo(get(l, "commit"));
    g.ledger.starts = index_pairs(get(l, "starts"));
    each_pair(get(j, "readers"), [&](const Json& k, const Json& v) { g.readers.insert(b(k), read_reader(v)); });
    each_pair(get(j, "pending"), [&](const Json& k, const Json& v) {
        Pending p;
        each_pair(get(v, "acks"), [&](const Json& sk, const Json& a) {
            auto& x = tup(a, 2);
            p.acks.insert(b(sk), AckedRun{ob(x[0]), b(x[1])});
        });
        for (auto& x : arr(get(v, "numbers"))) {
            auto& q = tup(x, 2);
            p.numbers.push_back({(size_t)u(q[0]), q[1].is_null() ? std::nullopt : std::optional<At>(read_at(q[1]))});
        }
        p.introduced = ob(get(v, "introduced"));
        p.control = bo(get(v, "control"));
        g.pending.insert(u(k), p);
    });
    auto& ia = get(j, "introduced_at");
    if (!ia.is_null()) g.introduced_at = IntroRef{(size_t)u(get(ia, "introducer")), b(get(ia, "index"))};
    each_pair(get(j, "received"), [&](const Json& k, const Json& v) {
        auto& p = tup(k, 2);
        g.received.insert(ReceivedAt{b(p[0]), b(p[1])}, u(v));
    });
    g.read_cap = b(get(j, "read_cap"));
    g.policy = read_retention(get(j, "policy"));
    g.recheck = u(get(j, "recheck"));
    g.epoch = u(get(j, "epoch"));
    g.pool = indexes_r(get(j, "pool"));
    g.tip = b(get(j, "tip"));
    g.deriving = bo(get(j, "deriving"));
    g.outbox = indexes_r(get(j, "outbox"));
    g.tokens = u(get(j, "tokens"));
    for (auto& a : arr(get(j, "unnumbered"))) g.unnumbered.push_back(read_at(a));
    g.joined_through = ob(get(j, "joined_through"));
    g.awaiting_start = index_pairs(get(j, "awaiting_start"));
    g.allocates = j["allocates"].kind == Json::BOOL ? j["allocates"].b() : true;
    return g;
}

}  // namespace kq1::trace
