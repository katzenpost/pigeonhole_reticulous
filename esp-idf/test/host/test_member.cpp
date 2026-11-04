// SPDX-License-Identifier: AGPL-3.0-only
// The katzenqt1 member against pigeonhole_protocols testdata/katzenqt1, as its Rust tests
// (tests/katzenqt1_member_sim.rs, katzenqt1_member_replay.rs) do: katzenqt's scenarios run as
// written, their calls match katzenqt's recorded traces, and every trace replays to the same
// results and states. The traces (4.9 MB) are read from KQ1_TESTDATA, a pigeonhole_protocols
// checkout's testdata/katzenqt1 (by default the one beside this repository).
#include <sodium.h>

#include <filesystem>
#include <functional>
#include <sstream>

#include "kq1_member.h"
#include "kq1_trace.h"
#include "vectors.h"

using namespace kq1;
using namespace kq1::trace;
namespace fs = std::filesystem;

// ---- trace JSON, as member/trace.rs ----

static Json effect(const Effect& e) {
    auto t = [](const char* s) { return Json::str(s); };
    switch (e.kind) {
        case Effect::WRITE_BOX: return Json::obj({{"type", t("WriteBox")}, {"index", h(e.index)}, {"payload", payload(e.payload)}});
        case Effect::WRITE_TOMBSTONE: return Json::obj({{"type", t("WriteTombstone")}, {"index", h(e.index)}});
        case Effect::READ_BOX:
            return Json::obj({{"type", t("ReadBox")}, {"stream", h(e.stream)}, {"read_cap", h(e.read_cap)}, {"index", h(e.index)},
                              {"recheck", Json::boolean(e.recheck)}});
        case Effect::DERIVE_INDEXES:
            return Json::obj({{"type", t("DeriveIndexes")}, {"stream", h(e.stream)}, {"start", h(e.start)}, {"count", nu(e.count)}});
        case Effect::WALK: {
            std::vector<Json> ts;
            for (auto x : e.targets) ts.push_back(nu(x));
            return Json::obj({{"type", t("Walk")}, {"stream", h(e.stream)}, {"start", h(e.start)}, {"targets", Json::arr(ts)},
                              {"keep", nu(e.keep)}});
        }
        case Effect::DELIVER:
            return Json::obj({{"type", t("Deliver")}, {"sender", h(e.sender)}, {"index", h(e.index)}, {"content", h(e.content)}});
        case Effect::EXPIRE: return Json::obj({{"type", t("Expire")}, {"sender", h(e.sender)}, {"index", h(e.index)}});
        case Effect::SEND_JOIN_REPLY: return Json::obj({{"type", t("SendJoinReply")}, {"to", h(e.to)}, {"reply", join_reply(e.reply)}});
    }
    return Json{};
}
static Json effects(const std::vector<Effect>& es) {
    std::vector<Json> a;
    for (auto& e : es) a.push_back(effect(e));
    return Json::arr(a);
}
static Json outgoing(const Outgoing& o) {
    return Json::obj({{"type", Json::str("Outgoing")}, {"token", nu(o.token)}, {"message", message(o.message)}});
}
static Json outgoings(const std::vector<Outgoing>& os) {
    std::vector<Json> a;
    for (auto& o : os) a.push_back(outgoing(o));
    return Json::arr(a);
}
static Json read_result_j(const ReadResult& r) {
    if (r.kind == ReadResult::NOT_FOUND) return Json::str("not found");
    if (r.kind == ReadResult::TOMBSTONE) return Json::str("tombstone");
    return Json::obj({{"data", r.message ? message(*r.message) : Json{}}});
}
static Json event_j(const Event& e) {
    auto t = [](const char* s) { return Json::str(s); };
    switch (e.kind) {
        case Event::SEND: return Json::obj({{"type", t("Send")}, {"content", h(e.content)}});
        case Event::READ: return Json::obj({{"type", t("Read")}, {"stream", h(e.stream)}});
        case Event::SCAN: return Json::obj({{"type", t("Scan")}, {"stream", h(e.stream)}});
        case Event::BOX_READ:
            return Json::obj({{"type", t("BoxRead")}, {"stream", h(e.stream)}, {"index", h(e.index)},
                              {"result", read_result_j(e.result)}, {"next_index", oh(e.next)}});
        case Event::INDEXES_DERIVED: return Json::obj({{"type", t("IndexesDerived")}, {"stream", h(e.stream)}, {"indexes", hexes(e.indexes)}});
        case Event::WALKED:
            return Json::obj({{"type", t("Walked")}, {"stream", h(e.stream)}, {"target", oh(e.walked.target)},
                              {"skipped", hexes(e.walked.skipped)}, {"steps", nu(e.walked.steps)}});
        case Event::INVITE: return Json::obj({{"type", t("Invite")}, {"name", Json::str(e.name)}, {"read_cap", h(e.read_cap)}});
        case Event::INTRODUCE: return Json::obj({{"type", t("Introduce")}, {"name", Json::str(e.name)}, {"read_cap", h(e.read_cap)}});
        case Event::REMOVE: return Json::obj({{"type", t("Remove")}, {"member", h(e.member)}});
        case Event::EPOCH_CHANGED: return Json::obj({{"type", t("EpochChanged")}, {"epoch", nu(e.epoch)}});
    }
    return Json{};
}
static Json args_j(const Group& g, const Json& more) {
    Json p = retention(g.policy);
    p.o["type"] = Json::str("Retention");
    Json a = Json::obj({{"policy", p}, {"recheck", nu(g.recheck)}});
    for (auto& [k, v] : more.o) a.o[k] = v;
    return a;
}
static Json line_j(const std::string& call, const Key& me, const std::string& name, const Json& args, const Json& result,
                   const Group& g) {
    return Json::obj({{"call", Json::str(call)}, {"me", h(me)}, {"member", Json::str(name)}, {"args", args},
                      {"result", result}, {"state", group_j(g)}});
}

static std::string dump(const Json& j) {
    switch (j.kind) {
        case Json::NUL: return "null";
        case Json::BOOL: case Json::NUM: return j.s;
        case Json::STR: return "\"" + j.s + "\"";
        case Json::ARR: {
            std::string s = "[";
            for (size_t i = 0; i < j.a.size(); i++) s += (i ? "," : "") + dump(j.a[i]);
            return s + "]";
        }
        case Json::OBJ: {
            std::string s = "{";
            bool first = true;
            for (auto& [k, v] : j.o) { s += (first ? "\"" : ",\"") + k + "\":" + dump(v); first = false; }
            return s + "}";
        }
    }
    return "";
}

static std::optional<std::string> diff(const std::string& path, const Json& ours, const Json& theirs) {
    if (ours.kind == Json::OBJ && theirs.kind == Json::OBJ) {
        std::vector<std::string> keys;
        for (auto& [k, _] : ours.o) keys.push_back(k);
        for (auto& [k, _] : theirs.o) if (!ours.o.count(k)) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (auto& k : keys)
            if (auto d = diff(path + "." + k, ours[k], theirs[k])) return d;
        return std::nullopt;
    }
    if (ours.kind == Json::ARR && theirs.kind == Json::ARR) {
        if (ours.a.size() != theirs.a.size())
            return path + ": " + std::to_string(ours.a.size()) + " items here, " + std::to_string(theirs.a.size()) +
                   " in the trace\n  here  " + dump(ours) + "\n  trace " + dump(theirs);
        for (size_t i = 0; i < ours.a.size(); i++)
            if (auto d = diff(path + "[" + std::to_string(i) + "]", ours.a[i], theirs.a[i])) return d;
        return std::nullopt;
    }
    if (ours == theirs) return std::nullopt;
    return path + ": here " + dump(ours) + ", in the trace " + dump(theirs);
}

// ---- reading traces ----

static ReadResult read_result_r(const Json& j) {
    if (j.kind == Json::STR && j.s == "not found") return ReadResult{ReadResult::NOT_FOUND, std::nullopt};
    if (j.kind == Json::STR && j.s == "tombstone") return ReadResult{ReadResult::TOMBSTONE, std::nullopt};
    return ReadResult{ReadResult::DATA, read_opt_message(get(j, "data"))};
}
static Event read_event(const Json& j) {
    const std::string& t = get(j, "type").s;
    auto f = [&](const char* k) -> const Json& { return get(j, k); };
    if (t == "Send") { Event e(Event::SEND); e.content = b(f("content")); return e; }
    if (t == "Read") { Event e(Event::READ); e.stream = b(f("stream")); return e; }
    if (t == "Scan") { Event e(Event::SCAN); e.stream = b(f("stream")); return e; }
    if (t == "BoxRead") {
        Event e(Event::BOX_READ);
        e.stream = b(f("stream")); e.index = b(f("index")); e.result = read_result_r(f("result")); e.next = ob(f("next_index"));
        return e;
    }
    if (t == "IndexesDerived") { Event e(Event::INDEXES_DERIVED); e.stream = b(f("stream")); e.indexes = indexes_r(f("indexes")); return e; }
    if (t == "Walked") {
        Event e(Event::WALKED);
        e.stream = b(f("stream")); e.walked.target = ob(f("target")); e.walked.skipped = indexes_r(f("skipped"));
        e.walked.steps = u(f("steps"));
        return e;
    }
    if (t == "Invite" || t == "Introduce") {
        Event e(t == "Invite" ? Event::INVITE : Event::INTRODUCE);
        e.name = f("name").s; e.read_cap = b(f("read_cap"));
        return e;
    }
    if (t == "Remove") { Event e(Event::REMOVE); e.member = b(f("member")); return e; }
    if (t == "EpochChanged") { Event e(Event::EPOCH_CHANGED); e.epoch = u(f("epoch")); return e; }
    throw Bad("unknown event " + t);
}

// One recorded call made again: its result JSON, the effects it returned, the group after it.
static std::tuple<Json, std::vector<Effect>, Group> call(std::optional<Group> g, const Json& line) {
    auto& a = get(line, "args");
    auto arg = [&](const char* k) -> const Json& { return get(a, k); };
    Retention policy = read_retention(arg("policy"));
    size_t recheck = u(arg("recheck"));
    const std::string& name = get(line, "call").s;
    if (g) g->policy = policy;
    if (name == "found") {
        return {Json{}, {}, Group::found(b(arg("read_cap")), indexes_r(arg("founders")), policy, recheck)};
    }
    if (name == "joined") return {Json{}, {}, Group::joined(b(arg("read_cap")), read_join_reply(arg("reply")), policy, recheck)};
    if (!g) throw Bad(name + " before the member started");
    Json result;
    if (name == "handle") {
        auto es = g->handle(read_event(arg("event")));
        return {effects(es), es, *g};
    } else if (name == "observe") {
        auto es = g->observe(b(arg("stream")), b(arg("index")), read_opt_message(arg("message")), b(arg("next")));
        return {effects(es), es, *g};
    } else if (name == "compose") {
        result = outgoings(g->compose(ob(arg("content"))));
    } else if (name == "written") {
        auto& p = arg("payload");
        g->written(u(arg("token")), b(arg("index")), p.is_null() ? std::nullopt : std::optional<Payload>(read_payload(p)));
    } else if (name == "abandoned") {
        g->abandoned(u(arg("token")));
    } else if (name == "begin_induction") {
        result = outgoings(g->begin_induction());
    } else if (name == "introduce_now") {
        result = outgoing(g->introduce_now(arg("name").s, b(arg("read_cap"))));
    } else if (name == "cancel_induction") {
        g->cancel_induction();
    } else if (name == "reply") {
        result = join_reply(g->reply(b(arg("own_start"))));
    } else if (name == "remove") {
        g->remove(b(arg("member")));
    } else {
        throw Bad("unknown call " + name);
    }
    return {result, {}, *g};
}

struct Asked {
    std::vector<std::pair<Index, size_t>> derives;
    std::vector<std::tuple<Key, Index, std::vector<uint64_t>, size_t>> walks;
};
static Index step(const Index& i) {
    auto n = next_index(i);
    if (!n) throw Bad("no index after");
    return *n;
}
static size_t check_bacap(Asked& asked, const Json& line, const std::vector<Effect>& es) {
    auto& a = get(line, "args");
    size_t checks = 0;
    const std::string& c = get(line, "call").s;
    if (c == "handle") {
        Event e = read_event(get(a, "event"));
        if (e.kind == Event::BOX_READ && e.next) {
            checks++;
            if (step(e.index) != *e.next) throw Bad("the index after a box read is not BACAP's");
        } else if (e.kind == Event::INDEXES_DERIVED) {
            if (asked.derives.empty()) throw Bad("indexes derived that were not asked for");
            auto [start, count] = asked.derives.front();
            asked.derives.erase(asked.derives.begin());
            checks++;
            if (derive_with(start, count) != std::optional<std::vector<Index>>(e.indexes)) throw Bad("the indexes derived are not BACAP's");
        } else if (e.kind == Event::WALKED) {
            auto it = std::find_if(asked.walks.begin(), asked.walks.end(), [&](auto& w) { return std::get<0>(w) == e.stream; });
            if (it == asked.walks.end()) throw Bad("a walk that was not asked for");
            auto [_, start, targets, keep] = *it;
            asked.walks.erase(it);
            checks++;
            auto w = walk_with(start, targets, keep);
            if (start.size() != 104 || !w || w->target != e.walked.target || w->skipped != e.walked.skipped ||
                w->steps != e.walked.steps)
                throw Bad("the walk's result is not BACAP's");
        }
    } else if (c == "observe") {
        checks++;
        if (step(b(get(a, "index"))) != b(get(a, "next"))) throw Bad("the index after an observed box is not BACAP's");
    }
    for (auto& e : es) {
        if (e.kind == Effect::DERIVE_INDEXES) asked.derives.push_back({e.start, e.count});
        if (e.kind == Effect::WALK) asked.walks.push_back({e.stream, e.start, e.targets, e.keep});
    }
    return checks;
}

static std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) if (!l.empty()) out.push_back(l);
    return out;
}

// Replays one trace; returns (calls, BACAP checks) or throws with the line that differs.
static std::pair<size_t, size_t> replay(const std::string& text) {
    OMap<std::string, std::pair<std::optional<Group>, Asked>> members;
    size_t calls = 0, checks = 0;
    for (auto& l : lines_of(text)) {
        calls++;
        Json line = Json::parse(l);
        std::string me = line["me"].s, who = line["member"].kind == Json::STR ? line["member"].s : me;
        std::string cn = line["call"].kind == Json::STR ? line["call"].s : "?";
        auto& slot = members.entry(me);
        try {
            auto [result, es, ng] = call(slot.first, line);
            if (auto d = diff("result", result, line["result"])) throw Bad(*d);
            if (auto d = diff("state", group_j(ng), line["state"])) throw Bad(*d);
            checks += check_bacap(slot.second, line, es);
            slot.first = ng;
        } catch (const Bad& e) {
            throw Bad("line " + std::to_string(calls) + ": " + who + " " + cn + ": " + e.what());
        }
    }
    return {calls, checks};
}

// ---- the scenario simulator, as member/sim.rs ----

static Bytes draw(const std::string& name, int n, size_t size) {
    std::string seed = name + "/" + std::to_string(n);
    crypto_xof_shake256_state st;
    crypto_xof_shake256_init(&st);
    crypto_xof_shake256_update(&st, (const uint8_t*)seed.data(), seed.size());
    Bytes out(size);
    crypto_xof_shake256_squeeze(&st, out.data(), size);
    return out;
}
static uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}
static Bytes read_cap_of(const std::string& name) {
    Bytes seed = draw(name, 1, 32), pk(32), sk(64);
    crypto_sign_ed25519_seed_keypair(pk.data(), sk.data(), seed.data());
    Bytes hkdf_state = draw(name, 2, 32), r = draw(name, 3, 16);
    r[7] &= 0x3f;
    r[15] &= 0x3f;
    uint64_t idx = le64(r.data()) + le64(r.data() + 8);
    Bytes index(8);
    for (int i = 0; i < 8; i++) index[i] = (uint8_t)(idx >> (8 * i));
    index.insert(index.end(), 64, 0);
    index.insert(index.end(), hkdf_state.begin(), hkdf_state.end());
    Bytes out = pk, n = *next_index(index);
    out.insert(out.end(), n.begin(), n.end());
    return out;
}
static Index stepd(const Index& i) { return next_index(i).value_or(Index{}); }

using Stored = std::optional<Message>;
struct Replica {
    size_t epoch = 0;
    std::vector<std::pair<size_t, std::vector<std::pair<Index, Stored>>>> buckets;
    std::vector<size_t> kept() const { return epoch == 0 ? std::vector<size_t>{0} : std::vector<size_t>{epoch, epoch - 1}; }
    const std::vector<std::pair<Index, Stored>>* bucket(size_t e) const {
        for (auto& b : buckets) if (b.first == e) return &b.second;
        return nullptr;
    }
    const Stored* read(const Index& index) const {
        for (size_t e : kept())
            if (auto b = bucket(e))
                for (auto& s : *b) if (s.first == index) return &s.second;
        return nullptr;
    }
    std::string write(const Index& index, Stored s) {
        if (s)
            if (auto old = read(index)) return *old == s ? "same" : "refused";
        std::vector<std::pair<Index, Stored>> nb;
        if (auto b = bucket(epoch))
            for (auto& x : *b) if (x.first != index) nb.push_back(x);
        nb.push_back({index, std::move(s)});
        buckets.erase(std::remove_if(buckets.begin(), buckets.end(), [&](auto& x) { return x.first == epoch; }), buckets.end());
        buckets.push_back({epoch, nb});
        return "stored";
    }
    void advance() {
        epoch++;
        auto k = kept();
        buckets.erase(std::remove_if(buckets.begin(), buckets.end(),
                                     [&](auto& x) { return std::find(k.begin(), k.end(), x.first) == k.end(); }),
                      buckets.end());
    }
};

struct SimMember {
    std::string name;
    Bytes read_cap;
    std::vector<Index> chain;
    std::optional<Group> group;
    std::vector<std::tuple<Key, Index, std::string>> inbox;
    std::vector<std::pair<std::pair<Key, Index>, Message>> seen;
    long long reported = -1;
    bool passive = false;
    std::vector<Outgoing> unwritten;
    Index next_own;
    OMap<Key, Index> cursors;
    Key key() const { return key_of(read_cap); }
    std::optional<size_t> ordinal_of(const Index& i) const {
        for (size_t n = 0; n < chain.size(); n++) if (chain[n] == i) return n;
        return std::nullopt;
    }
    std::optional<size_t> ordinal_at(uint64_t idx64) const {
        uint64_t first = counter(chain[0]);
        if (first <= idx64 && idx64 - first < WALK_LIMIT) return (size_t)(idx64 - first);
        return std::nullopt;
    }
};

struct Opt { enum { FLAG, NOTHING, NUM } kind; bool flag = false; size_t num = 0; };
using Recorder = std::function<void(const Json&)>;
using Lines = std::vector<std::string>;

struct Fetched {
    enum Kind { DATA, TOMBSTONE, NOT_FOUND } kind;
    Message m;
    const char* value() const { return kind == TOMBSTONE ? "tombstone" : "not found"; }
    ReadResult result() const {
        if (kind == DATA) return ReadResult{ReadResult::DATA, m};
        return ReadResult{kind == TOMBSTONE ? ReadResult::TOMBSTONE : ReadResult::NOT_FOUND, std::nullopt};
    }
};

struct World {
    Replica replica;
    std::vector<SimMember> members;
    std::vector<std::pair<std::string, Opt>> options;
    Recorder record;

    const Opt* option(const std::string& n) const {
        for (auto& o : options) if (o.first == n) return &o.second;
        return nullptr;
    }
    bool flag(const std::string& n) const { auto o = option(n); return o && o->kind == Opt::FLAG && o->flag; }
    Retention policy() const {
        auto num = [&](const char* n, std::optional<size_t> d) -> std::optional<size_t> {
            auto o = option(n);
            if (o && o->kind == Opt::NUM) return o->num;
            if (o && o->kind == Opt::NOTHING) return std::nullopt;
            return d;
        };
        Retention r;
        r.window = num("window", 3);
        r.keep_sent = num("keep_sent", std::nullopt);
        r.keep_received = num("keep_received", std::nullopt);
        auto d = option("discard_acked");
        r.discard_acked = !(d && d->kind == Opt::FLAG && !d->flag);
        return r;
    }
    size_t recheck() const { auto o = option("recheck"); return o && o->kind == Opt::NUM ? o->num : 256; }

    size_t new_member(const std::string& name) {
        SimMember m;
        m.name = name;
        m.read_cap = read_cap_of(name);
        m.chain = {Bytes(m.read_cap.begin() + 32, m.read_cap.end())};
        while (m.chain.size() < WALK_LIMIT) m.chain.push_back(stepd(m.chain.back()));
        for (size_t i = 0; i < members.size(); i++)
            if (members[i].name == name) { members[i] = m; return i; }
        members.push_back(m);
        return members.size() - 1;
    }
    size_t member(const std::string& name) {
        for (size_t i = 0; i < members.size(); i++) if (members[i].name == name) return i;
        return new_member(name);
    }
    size_t existing(const std::string& name) const {
        for (size_t i = 0; i < members.size(); i++) if (members[i].name == name) return i;
        throw Bad("no member " + name);
    }
    Group& group_of(size_t i) {
        if (!members[i].group) throw Bad(members[i].name + " has not joined");
        return *members[i].group;
    }
    std::optional<size_t> by_key(const Bytes& k) const {
        for (size_t i = 0; i < members.size(); i++) if (members[i].key() == k) return i;
        return std::nullopt;
    }
    std::string name_of(const std::optional<Key>& k) const {
        if (!k) return "?";
        static const Bytes retired{'r', 'e', 't', 'i', 'r', 'e', 'd'};
        if (k->size() >= 7 && std::equal(retired.begin(), retired.end(), k->begin())) return "removed";
        auto i = by_key(*k);
        return i ? members[*i].name : "?";
    }
    std::string at_index(const Bytes& k, const Index& index) const {
        auto i = by_key(k);
        if (!i) throw Bad("no member with that key");
        auto n = members[*i].ordinal_of(index);
        return members[*i].name + "@" + (n ? std::to_string(*n) : "None");
    }
    std::string at_idx64(const Bytes& k, uint64_t idx64) const {
        auto i = by_key(k);
        if (!i) throw Bad("no member with that key");
        auto n = members[*i].ordinal_at(idx64);
        return members[*i].name + "@" + (n ? std::to_string(*n) : "None");
    }
    std::string where_idx64(uint64_t idx64) const {
        for (auto& m : members)
            if (auto n = m.ordinal_at(idx64)) return m.name + "@" + std::to_string(*n);
        return "?";
    }
    std::string prefixed(const Bytes& p) const {
        const SimMember* found = nullptr;
        int count = 0;
        for (auto& m : members) {
            Key k = m.key();
            if (k.size() >= p.size() && std::equal(p.begin(), p.end(), k.begin())) { found = &m; count++; }
        }
        return count == 1 ? found->name : "?";
    }
    std::string introduced(const Bytes& index) const {
        for (auto& m : members) {
            if (!m.group) continue;
            auto r = m.group->sent.records.get(counter(index));
            if (r && r->index == index && r->plaintext && !r->plaintext->is_bytes && r->plaintext->message.introduction)
                return r->plaintext->message.introduction->name;
        }
        return "?";
    }
    std::string show_msg(const Message& m) const {
        std::vector<std::string> parts;
        if (m.content) parts.push_back("\"" + std::string(m.content->begin(), m.content->end()) + "\"");
        if (m.introduction) parts.push_back("introduces " + m.introduction->name);
        if (m.introduced_at) parts.push_back("introduced-at " + introduced(m.introduced_at->index));
        if (!m.numbers.empty()) {
            std::string s = "numbers";
            for (auto& n : m.numbers) {
                std::string who = "?";
                if (n.prefix) who = n.prefix->empty() ? "?" : prefixed(*n.prefix);
                else if (m.introduction) who = m.introduction->name;
                s += " " + std::to_string(n.index) + ":" + who;
            }
            parts.push_back(s);
        }
        if (!m.acks.empty()) {
            auto acks = m.acks;
            std::stable_sort(acks.begin(), acks.end(), [](const Ack& a, const Ack& b) { return a.member < b.member; });
            std::string s = "acks";
            for (auto& a : acks) s += " " + std::to_string(a.member) + ":" + where_idx64(a.idx64);
            parts.push_back(s);
        }
        if (parts.empty()) return "(empty)";
        std::string out;
        for (size_t i = 0; i < parts.size(); i++) out += (i ? " " : "") + parts[i];
        return out;
    }
    std::pair<Fetched, std::optional<Index>> fetch(const Index& index) const {
        auto s = replica.read(index);
        if (!s) return {Fetched{Fetched::NOT_FOUND, {}}, std::nullopt};
        if (!*s) return {Fetched{Fetched::TOMBSTONE, {}}, stepd(index)};
        return {Fetched{Fetched::DATA, **s}, stepd(index)};
    }
    void store_ok(const Index& index, Stored s) {
        auto r = replica.write(index, std::move(s));
        if (r != "stored" && r != "same") throw Bad("a write was " + r);
    }
    Lines show_read(size_t i, const Key& stream, const Index& index, const Fetched& f, bool recheck) {
        std::string at = at_index(stream, index);
        if (f.kind == Fetched::DATA) {
            std::string shown = at + " " + show_msg(f.m);
            auto& seen = members[i].seen;
            std::pair<Key, Index> k{stream, index};
            seen.erase(std::remove_if(seen.begin(), seen.end(), [&](auto& e) { return e.first == k; }), seen.end());
            seen.push_back({k, f.m});
            return {shown};
        }
        if (recheck) return {at + " recheck: " + f.value()};
        Lines lines{at + " " + f.value()};
        auto o = by_key(stream);
        if (!o) return lines;
        auto here = members[*o].ordinal_of(index);
        if (f.kind == Fetched::NOT_FOUND && here)
            if (auto h = group_of(i).ledger.heard.get(stream))
                for (auto& [by, heard] : *h) {
                    auto there = members[*o].ordinal_at(heard);
                    if (there && *there >= *here)
                        lines.push_back("  stuck: " + name_of(by) + " acked " + at_idx64(stream, heard));
                }
        return lines;
    }
    void rec(const std::string& c, size_t i, const Json& more, const Json& result) {
        if (!record) return;
        auto& m = members[i];
        record(line_j(c, m.key(), m.name, args_j(*m.group, more), result, *m.group));
    }
    std::vector<Effect> handle(size_t i, const Event& e) {
        Json ev = event_j(e);
        if (!members[i].group) throw Bad("not joined");
        auto es = members[i].group->handle(e);
        rec("handle", i, Json::obj({{"event", ev}}), effects(es));
        return es;
    }
    void make_passive(size_t i) {
        if (!flag("passive")) return;
        members[i].passive = true;
        members[i].next_own = members[i].chain[0];
    }
    Lines join(size_t i, const JoinReply& reply) {
        if (members[i].group) return {members[i].name + " already joined"};
        members[i].group = Group::joined(members[i].read_cap, reply, policy(), recheck());
        rec("joined", i, Json::obj({{"read_cap", h(members[i].read_cap)}, {"reply", join_reply(reply)}}), Json{});
        Event e(Event::EPOCH_CHANGED);
        e.epoch = replica.epoch;
        handle(i, e);
        make_passive(i);
        return {};
    }
    Lines drive(size_t i, const Event& event) {
        Lines out;
        auto pending = handle(i, event);
        while (!pending.empty()) {
            Effect e = pending.front();
            pending.erase(pending.begin());
            std::optional<Event> follow;
            switch (e.kind) {
                case Effect::WRITE_BOX:
                    if (e.payload.is_bytes) throw Bad("a member asked to write bytes it was never given");
                    store_ok(e.index, e.payload.message);
                    break;
                case Effect::WRITE_TOMBSTONE: store_ok(e.index, std::nullopt); break;
                case Effect::READ_BOX: {
                    auto [f, next] = fetch(e.index);
                    auto l = show_read(i, e.stream, e.index, f, e.recheck);
                    out.insert(out.end(), l.begin(), l.end());
                    Event x(Event::BOX_READ);
                    x.stream = e.stream;
                    x.index = e.index;
                    x.result = f.result();
                    x.next = next;
                    follow = x;
                    break;
                }
                case Effect::DERIVE_INDEXES: {
                    std::vector<Index> is;
                    for (size_t k = 0; k < std::max<size_t>(e.count, 1); k++) is.push_back(stepd(is.empty() ? e.start : is.back()));
                    Event x(Event::INDEXES_DERIVED);
                    x.stream = e.stream;
                    x.indexes = is;
                    follow = x;
                    break;
                }
                case Effect::WALK: {
                    Event x(Event::WALKED);
                    x.stream = e.stream;
                    x.walked = *walk_with(e.start, e.targets, e.keep);
                    follow = x;
                    break;
                }
                case Effect::DELIVER:
                    members[i].inbox.push_back({e.sender, e.index, std::string(e.content.begin(), e.content.end())});
                    break;
                case Effect::EXPIRE: {
                    auto& in = members[i].inbox;
                    in.erase(std::remove_if(in.begin(), in.end(),
                                            [&](auto& d) { return std::get<0>(d) == e.sender && std::get<1>(d) == e.index; }),
                             in.end());
                    break;
                }
                case Effect::SEND_JOIN_REPLY: {
                    auto n = by_key(key_of(e.to));
                    if (!n) throw Bad("a join reply to no member");
                    auto l = join(*n, e.reply);
                    out.insert(out.end(), l.begin(), l.end());
                    break;
                }
            }
            if (follow) {
                auto more = handle(i, *follow);
                more.insert(more.end(), pending.begin(), pending.end());
                pending = more;
            }
        }
        return out;
    }
    void written(size_t i, size_t token, const Index& index, const Payload& p) {
        Json pj = payload(p);
        members[i].group->written(token, index, p);
        rec("written", i, Json::obj({{"token", nu(token)}, {"index", h(index)}, {"payload", pj}}), Json{});
    }
    void land(size_t i) {
        auto out = std::move(members[i].unwritten);
        members[i].unwritten.clear();
        for (auto& o : out) {
            Index at = members[i].next_own;
            store_ok(at, o.message);
            written(i, o.token, at, Payload::of(o.message));
            members[i].next_own = stepd(at);
        }
    }
    void queue(size_t i, const std::vector<Outgoing>& out) {
        auto& u = members[i].unwritten;
        u.insert(u.end(), out.begin(), out.end());
        if (!flag("delay_writes")) land(i);
    }
    Lines written_since(size_t i) {
        Group& g = group_of(i);
        auto ks = g.sent.records.keys();
        std::sort(ks.begin(), ks.end());
        Lines lines;
        long long reported = members[i].reported;
        for (auto k : ks) {
            auto& r = *g.sent.records.get(k);
            if ((long long)k > reported && r.plaintext && !r.plaintext->is_bytes) {
                lines.push_back(at_index(members[i].key(), r.index) + " " + show_msg(r.plaintext->message));
                reported = (long long)k;
            }
        }
        members[i].reported = reported;
        return lines;
    }
    Lines read_passively(size_t i, size_t owner) {
        Key okey = members[owner].key();
        auto r = group_of(i).readers.get(okey);
        if (!r) return {};
        Index cursor = members[i].cursors.get(okey) ? *members[i].cursors.get(okey) : r->next;
        Lines out;
        for (;;) {
            auto [f, next] = fetch(cursor);
            auto l = show_read(i, okey, cursor, f, false);
            out.insert(out.end(), l.begin(), l.end());
            if (!next) break;
            std::optional<Message> m;
            if (f.kind == Fetched::DATA) m = f.m;
            Json mj = m ? message(*m) : Json{};
            auto es = members[i].group->observe(okey, cursor, m, *next);
            rec("observe", i, Json::obj({{"stream", h(okey)}, {"index", h(cursor)}, {"message", mj}, {"next", h(*next)}}), effects(es));
            for (auto& e : es)
                if (e.kind == Effect::DELIVER)
                    members[i].inbox.push_back({e.sender, e.index, std::string(e.content.begin(), e.content.end())});
            cursor = *next;
        }
        members[i].cursors.insert(okey, cursor);
        return out;
    }
    std::vector<Outgoing> compose(size_t i, std::optional<Bytes> content) {
        Json cj = content ? h(*content) : Json{};
        auto out = members[i].group->compose(content);
        rec("compose", i, Json::obj({{"content", cj}}), outgoings(out));
        return out;
    }
    Lines invite(const std::string& who, const std::string& added) {
        size_t i = existing(who);
        if (members[i].passive) {
            land(i);
            auto out = group_of(i).begin_induction();
            rec("begin_induction", i, Json::obj({}), outgoings(out));
            queue(i, out);
            land(i);
            return written_since(i);
        }
        size_t n = member(added);
        Event e(Event::INVITE);
        e.name = added;
        e.read_cap = members[n].read_cap;
        auto out = drive(i, e);
        auto w = written_since(i);
        out.insert(out.end(), w.begin(), w.end());
        return out;
    }
    Lines introduce(const std::string& who, const std::string& added) {
        size_t i = existing(who);
        if (members[i].passive) {
            if (group_of(i).ledger.induction != Induction::INDUCTING) invite(who, added);
            Index own = members[i].next_own;
            auto reply = group_of(i).reply(own);
            rec("reply", i, Json::obj({{"own_start", h(own)}}), join_reply(reply));
            size_t n = member(added);
            Bytes rc = members[n].read_cap;
            auto o = members[i].group->introduce_now(added, rc);
            rec("introduce_now", i, Json::obj({{"name", Json::str(added)}, {"read_cap", h(rc)}}), outgoing(o));
            queue(i, {o});
            land(i);
            auto out = join(n, reply);
            auto w = written_since(i);
            out.insert(out.end(), w.begin(), w.end());
            return out;
        }
        size_t n = member(added);
        Event e(Event::INTRODUCE);
        e.name = added;
        e.read_cap = members[n].read_cap;
        auto out = drive(i, e);
        auto w = written_since(i);
        out.insert(out.end(), w.begin(), w.end());
        return out;
    }
    std::vector<std::string> joined_names() const {
        std::vector<std::string> ns;
        for (auto& m : members) if (m.group) ns.push_back(m.name);
        std::sort(ns.begin(), ns.end());
        return ns;
    }
    Lines backfill(const std::string& who) {
        size_t i = existing(who);
        Event e(Event::EPOCH_CHANGED);
        e.epoch = replica.epoch;
        return drive(i, e);
    }
    static size_t parse(const std::string& s) {
        size_t v = 0;
        for (char c : s) { if (!isdigit((unsigned char)c)) return 0; v = v * 10 + (c - '0'); }
        return v;
    }
    Lines command(const std::vector<std::string>& w) {
        auto is = [&](const char* c, size_t n) { return w.size() == n && w[0] == c; };
        if (is("option", 3)) {
            if (!members.empty()) throw Bad("set options before new-group");
            Opt v;
            if (w[2] == "on") v = Opt{Opt::FLAG, true};
            else if (w[2] == "off") v = Opt{Opt::FLAG, false};
            else if (w[2] == "none") v = Opt{Opt::NOTHING};
            else v = Opt{Opt::NUM, false, parse(w[2])};
            std::string o = w[1];
            std::replace(o.begin(), o.end(), '-', '_');
            options.insert(options.begin(), {o, v});
            return {};
        }
        if (!w.empty() && w[0] == "new-group") {
            std::vector<size_t> founders;
            for (size_t k = 1; k < w.size(); k++) founders.push_back(new_member(w[k]));
            std::vector<Bytes> caps;
            for (size_t f : founders) caps.push_back(members[f].read_cap);
            for (size_t f : founders) {
                members[f].group = Group::found(members[f].read_cap, caps, policy(), recheck());
                std::vector<Json> fs;
                for (auto& c : caps) fs.push_back(h(c));
                rec("found", f, Json::obj({{"read_cap", h(members[f].read_cap)}, {"founders", Json::arr(fs)}}), Json{});
                make_passive(f);
            }
            return {};
        }
        if (w.size() >= 2 && w[0] == "send") {
            size_t i = existing(w[1]);
            std::string text;
            for (size_t k = 2; k < w.size(); k++) text += (k > 2 ? " " : "") + w[k];
            Bytes content(text.begin(), text.end());
            if (members[i].passive) {
                queue(i, compose(i, content));
                return written_since(i);
            }
            Event e(Event::SEND);
            e.content = content;
            auto out = drive(i, e);
            auto ws = written_since(i);
            out.insert(out.end(), ws.begin(), ws.end());
            return out;
        }
        if (is("invite", 3)) return invite(w[1], w[2]);
        if (is("introduce", 3)) return introduce(w[1], w[2]);
        if (is("land", 2)) { land(existing(w[1])); return {}; }
        if (is("abandon", 2)) {
            size_t i = existing(w[1]);
            auto control = [](const Message& m) { return m.introduction || !m.numbers.empty() || m.introduced_at; };
            auto& u = members[i].unwritten;
            for (auto it = u.rbegin(); it != u.rend(); ++it) {
                if (control(it->message)) continue;
                size_t token = it->token;
                u.erase(std::remove_if(u.begin(), u.end(), [&](auto& x) { return x.token == token; }), u.end());
                group_of(i).abandoned(token);
                rec("abandoned", i, Json::obj({{"token", nu(token)}}), Json{});
                break;
            }
            return {};
        }
        if (is("remove", 3)) {
            size_t i = existing(w[1]);
            Event e(Event::REMOVE);
            e.member = members[existing(w[2])].key();
            return drive(i, e);
        }
        if (w.size() == 3 && (w[0] == "read" || w[0] == "scan")) {
            size_t i = existing(w[1]), o = existing(w[2]);
            if (members[i].passive) return read_passively(i, o);
            Event e(w[0] == "read" ? Event::READ : Event::SCAN);
            e.stream = members[o].key();
            return drive(i, e);
        }
        auto count = [&]() { return w.size() > 1 ? parse(w[1]) : 1; };
        if (!w.empty() && w[0] == "epoch") {
            for (size_t k = 0, n = count(); k < n; k++) replica.advance();
            return {};
        }
        if (is("backfill", 2)) return backfill(w[1]);
        if (!w.empty() && w[0] == "tick") {
            Lines out;
            for (size_t k = 0, n = count(); k < n; k++) {
                replica.advance();
                for (auto& name : joined_names()) {
                    auto l = backfill(name);
                    out.insert(out.end(), l.begin(), l.end());
                }
            }
            return out;
        }
        if (is("roster", 3)) {
            auto known = group_of(existing(w[1])).rosters.known(members[existing(w[2])].key());
            if (known.empty()) return {"unknown"};
            size_t top = 0;
            for (auto& e : known) top = std::max(top, e.index);
            std::string s;
            for (size_t n = 0; n <= top; n++) {
                std::optional<Key> k;
                for (auto& e : known) if (e.index == n) { k = e.key; break; }
                s += (n ? " " : "") + name_of(k);
            }
            return {s};
        }
        if (is("acks", 4)) {
            size_t wi = existing(w[1]);
            const SimMember& sender = members[existing(w[2])];
            Index at = sender.chain[parse(w[3])];
            const Message* msg = nullptr;
            for (auto& s : members[wi].seen)
                if (s.first == std::make_pair(sender.key(), at)) { msg = &s.second; break; }
            if (!msg) return {"not read"};
            Group& g = group_of(wi);
            auto acks = msg->acks;
            std::stable_sort(acks.begin(), acks.end(), [](const Ack& a, const Ack& b) { return a.member < b.member; });
            std::string s;
            for (size_t k = 0; k < acks.size(); k++)
                s += (k ? " " : "") + name_of(g.resolve(sender.key(), acks[k].member)) + "=" + where_idx64(acks[k].idx64);
            return {s.empty() ? "none" : s};
        }
        if (is("acked", 2)) {
            size_t i = existing(w[1]);
            Group& g = group_of(i);
            std::vector<std::pair<Key, uint64_t>> acked(g.ledger.acked_by.begin(), g.ledger.acked_by.end());
            std::stable_sort(acked.begin(), acked.end(), [](auto& a, auto& b) { return a.first < b.first; });
            std::string s;
            for (auto& [k, idx64] : acked)
                if (auto r = g.sent.records.get(idx64))
                    s += (s.empty() ? "" : " ") + name_of(k) + ":" + at_index(members[i].key(), r->index);
            return {s.empty() ? "none" : s};
        }
        if (is("inbox", 2)) {
            auto& m = members[existing(w[1])];
            Lines lines;
            for (auto& [s, x, t] : m.inbox) lines.push_back(at_index(s, x) + " \"" + t + "\"");
            if (lines.empty()) return {"empty"};
            return lines;
        }
        if (is("peek", 3)) {
            auto& o = members[existing(w[1])];
            auto [f, _] = fetch(o.chain[parse(w[2])]);
            return {f.kind == Fetched::DATA ? show_msg(f.m) : f.value()};
        }
        std::string all;
        for (auto& x : w) all += x + " ";
        throw Bad("unknown command " + all);
    }
};

static std::vector<std::string> words(const std::string& cmd) {
    std::vector<std::string> out;
    std::istringstream in(cmd);
    for (std::string x; in >> x;) out.push_back(x);
    return out;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string run(const std::string& script, Recorder record) {
    World w;
    w.record = std::move(record);
    std::vector<std::string> lines;
    size_t p = 0, q;
    while ((q = script.find('\n', p)) != std::string::npos) { lines.push_back(script.substr(p, q - p)); p = q + 1; }
    if (p < script.size()) lines.push_back(script.substr(p));
    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size();) {
        const std::string& line = lines[i];
        out.push_back(line);
        i++;
        std::string cmd = trim(line.substr(0, line.find('#')));
        if (cmd.empty()) continue;
        auto printed = w.command(words(cmd));
        if (i < lines.size() && lines[i] == "----") {
            out.push_back("----");
            i++;
            while (i < lines.size() && !trim(lines[i]).empty()) i++;
            out.insert(out.end(), printed.begin(), printed.end());
        }
    }
    std::string s;
    for (auto& l : out) s += l + "\n";
    return s;
}

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { fprintf(stderr, "cannot read %s\n", p.c_str()); exit(2); }
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static std::vector<fs::path> files_in(const fs::path& dir, const std::string& ext) {
    std::vector<fs::path> out;
    if (!fs::is_directory(dir)) { fprintf(stderr, "no directory %s (set KQ1_TESTDATA)\n", dir.c_str()); exit(2); }
    for (auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ext) out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

int main(int, char**) {
    if (sodium_init() < 0) return 2;
    const char* env = getenv("KQ1_TESTDATA");
    fs::path data = env ? env : "../../../../pigeonhole_protocols/testdata/katzenqt1";
    Vec v{"member", "", {}};

    auto scenarios = files_in(data / "groupchat_scenarios", ".txt");
    check(scenarios.size() == 14, v, "14 scenarios");
    for (auto& f : scenarios) {
        Vec s{"scenario", f.stem().string(), {}};
        std::string script = slurp(f);
        try {
            std::string got = run(script, nullptr);
            if (got != script) {
                size_t line = 1;
                for (size_t k = 0; k < std::min(got.size(), script.size()) && got[k] == script[k]; k++) line += got[k] == '\n';
                fprintf(stderr, "%s differs at line %zu\n", f.c_str(), line);
            }
            check(got == script, s, "runs as written");
            std::vector<Json> ours;
            run(script, [&](const Json& l) { ours.push_back(l); });
            auto theirs = lines_of(slurp(data / "groupchat_traces" / ("scenario-" + f.stem().string() + ".jsonl")));
            check(ours.size() == theirs.size(), s, "as many calls as katzenqt's trace");
            for (size_t k = 0; k < std::min(ours.size(), theirs.size()); k++)
                if (auto d = diff(f.stem().string() + ":" + std::to_string(k + 1), ours[k], Json::parse(theirs[k]))) {
                    fprintf(stderr, "%s\n", d->c_str());
                    check(false, s, "the calls katzenqt made");
                    break;
                }
        } catch (const Bad& e) {
            fprintf(stderr, "%s: %s\n", f.c_str(), e.what());
            check(false, s, "runs");
        }
    }

    auto traces = files_in(data / "groupchat_traces", ".jsonl");
    check(traces.size() == 20, v, "20 traces");
    size_t calls = 0, bacap = 0, states = 0;
    for (auto& f : traces) {
        Vec t{"trace", f.filename().string(), {}};
        std::string text = slurp(f);
        try {
            auto [n, c] = replay(text);
            calls += n;
            bacap += c;
            check(true, t, "replays");
            for (auto& l : lines_of(text)) {
                Json line = Json::parse(l);
                auto d = diff("state", group_j(read_group(line["state"])), line["state"]);
                if (d) fprintf(stderr, "%s: %s\n", f.c_str(), d->c_str());
                check(!d, t, "state round trip");
                states++;
            }
        } catch (const Bad& e) {
            fprintf(stderr, "%s: %s\n", f.c_str(), e.what());
            check(false, t, "replays");
        }
    }
    printf("member: %zu traces, %zu calls, %zu BACAP checks, %zu states\n", traces.size(), calls, bacap, states);
    return finish("member");
}
