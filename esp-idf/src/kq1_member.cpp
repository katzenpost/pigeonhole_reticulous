#include "kq1_member.h"

#include "kq_bacap.h"

namespace kq1 {

bool operator==(const Message& a, const Message& b) {
    return a.content == b.content && a.introduction == b.introduction && a.numbers == b.numbers &&
           a.introduced_at == b.introduced_at && a.acks == b.acks && a.membership_hash == b.membership_hash;
}

std::optional<Index> next_index(const Index& i) {
    auto m = kq::bacap::Index::from(i.data(), i.size());
    if (!m) return std::nullopt;
    auto n = m->next();
    if (!n) return std::nullopt;
    return n->bytes();
}

std::optional<WalkResult> walk_with(const Index& start, const std::vector<uint64_t>& targets, size_t keep) {
    uint64_t here = counter(start);
    std::optional<uint64_t> target;
    for (uint64_t t : targets)
        if (t >= here && t - here < WALK_LIMIT && (!target || t > *target)) target = t;
    WalkResult w;
    if (!target) return w;
    uint64_t first = keep != 0 && *target - here > keep ? *target - keep : here;
    Index index = start;
    for (uint64_t i = here; i < first; i++) {
        auto n = next_index(index);
        if (!n) return std::nullopt;
        index = *n;
    }
    for (uint64_t i = first; i < *target; i++) {
        w.skipped.push_back(index);
        auto n = next_index(index);
        if (!n) return std::nullopt;
        index = *n;
    }
    w.target = index;
    w.steps = (size_t)(*target - here);
    return w;
}

std::optional<std::vector<Index>> derive_with(const Index& start, size_t count) {
    std::vector<Index> out;
    Index at = start;
    for (size_t i = 0; i < count; i++) {
        auto n = next_index(at);
        if (!n) return std::nullopt;
        at = *n;
        out.push_back(at);
    }
    return out;
}

// ---- reader ----

Reader Reader::make(const Bytes& read_cap, Index next) {
    Reader r;
    r.stream = key_of(read_cap);
    r.read_cap = read_cap;
    r.next = std::move(next);
    return r;
}

Effect Reader::read_box(const Index& index, bool recheck) const {
    Effect e(Effect::READ_BOX);
    e.stream = stream;
    e.read_cap = read_cap;
    e.index = index;
    e.recheck = recheck;
    return e;
}

std::vector<Effect> Reader::read() {
    if (mode != Mode::IDLE) return {};
    mode = Mode::READING;
    return {read_box(next, false)};
}

std::vector<uint64_t> Reader::ahead(const std::vector<uint64_t>& heard) const {
    uint64_t here = counter(next);
    std::vector<uint64_t> out;
    for (uint64_t h : heard)
        if (h > here && std::find(tried.begin(), tried.end(), h) == tried.end() &&
            std::find(out.begin(), out.end(), h) == out.end())
            out.push_back(h);
    return out;
}

std::vector<Effect> Reader::scan(const std::vector<uint64_t>& heard, size_t window) {
    uint64_t here = counter(next);
    tried.erase(std::remove_if(tried.begin(), tried.end(),
                               [&](uint64_t t) {
                                   return !(t > here && std::find(heard.begin(), heard.end(), t) != heard.end());
                               }),
                tried.end());
    auto a = ahead(heard);
    std::vector<Effect> es;
    for (auto& k : kept) es.push_back(read_box(k.index, true));
    mode = Mode::SCANNING;
    if (a.empty()) {
        es.push_back(read_box(next, false));
        return es;
    }
    tried.insert(tried.end(), a.begin(), a.end());
    Effect w(Effect::WALK);
    w.stream = stream;
    w.start = next;
    w.targets = a;
    w.keep = window;
    es.push_back(w);
    return es;
}

std::optional<Index> Reader::run() const {
    std::optional<std::pair<size_t, std::optional<Index>>> min;
    for (auto& k : kept)
        if (!min || k.passed < min->first) min = {{k.passed, k.before}};
    if (lost && (!min || lost->passed < min->first)) min = {{lost->passed, lost->before}};
    if (!min) return last;
    return min->second;
}

void Reader::lose(size_t at, std::optional<Index> before) {
    if (!lost || at < lost->passed) lost = Lost{at, std::move(before)};
}

void Reader::forget(size_t window) {
    std::vector<Kept> dropped, keep;
    for (auto& k : kept) (k.passed + window < passed ? dropped : keep).push_back(k);
    for (auto& k : dropped) lose(k.passed, k.before);
    kept = keep;
}

void Reader::jump(const Index& t, const std::vector<Index>& skipped, size_t steps) {
    size_t start = passed + steps - skipped.size();
    if (start > passed) lose(passed, last);
    std::optional<Index> before = last;
    for (size_t n = 0; n < skipped.size(); n++) {
        kept.push_back(Kept{skipped[n], start + n, before});
        before = skipped[n];
    }
    passed += steps;
    next = t;
}

std::vector<Effect> Reader::walked(const WalkResult& w, size_t window) {
    if (!w.target) return {read_box(next, false)};
    jump(*w.target, w.skipped, w.steps);
    forget(window);
    return {read_box(*w.target, false)};
}

std::pair<std::optional<Message>, std::vector<Effect>> Reader::found(const Index& index, const ReadResult& result,
                                                                     const std::optional<Index>& nxt, size_t window) {
    auto message_of = [&]() -> std::optional<Message> {
        return result.kind == ReadResult::DATA ? result.message : std::nullopt;
    };
    for (size_t n = 0; n < kept.size(); n++) {
        if (kept[n].index != index) continue;
        if (result.kind == ReadResult::NOT_FOUND) return {std::nullopt, {}};
        kept.erase(kept.begin() + n);
        return {message_of(), {}};
    }
    if (index != next || mode == Mode::IDLE) return {std::nullopt, {}};
    if (!nxt || result.kind == ReadResult::NOT_FOUND) {
        mode = Mode::IDLE;
        return {std::nullopt, {}};
    }
    last = index;
    next = *nxt;
    passed += 1;
    forget(window);
    return {message_of(), {read_box(*nxt, false)}};
}

// ---- rosters ----

static bool starts_with(const Bytes& k, const Bytes& p) {
    return k.size() >= p.size() && std::equal(p.begin(), p.end(), k.begin());
}

void Rosters::learn(const Key& k) {
    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
}

std::optional<Key> Rosters::named(const Bytes& p) const {
    std::optional<Key> found;
    for (auto& k : keys) {
        if (!starts_with(k, p)) continue;
        if (found) return std::nullopt;
        found = k;
    }
    return found;
}

void Rosters::found(const std::vector<Key>& founders) {
    auto sorted = founders;
    std::sort(sorted.begin(), sorted.end());
    RosterMap roster;
    for (size_t i = 0; i < sorted.size(); i++) roster.insert(i, Entry{sorted[i], std::nullopt});
    for (auto& f : founders) {
        entries.insert(f, roster);
        learn(f);
    }
}

std::optional<Key> Rosters::resolve(const Key& owner_in, size_t index) const {
    const Key* owner = &owner_in;
    size_t fuel = started.size() + 1;
    std::vector<const Key*> seen;
    for (;;) {
        if (auto r = entries.get(*owner))
            if (auto e = r->get(index)) {
                if (e->key) return e->key;
                if (e->prefix && !e->prefix->empty()) return named(*e->prefix);
                return std::nullopt;
            }
        if (fuel == 0) return std::nullopt;
        fuel--;
        auto s = started.get(*owner);
        if (!s) return std::nullopt;
        if (index > s->top) return std::nullopt;
        for (auto* k : seen) if (*k == *owner) return std::nullopt;
        seen.push_back(owner);
        owner = &s->introducer;
    }
}

std::vector<size_t> Rosters::indexes(size_t fuel, std::vector<Key>& seen, const Key& owner) const {
    std::vector<size_t> own;
    if (auto r = entries.get(owner)) for (auto& p : *r) own.push_back(p.first);
    auto s = started.get(owner);
    if (!s) return own;
    if (fuel == 0 || std::find(seen.begin(), seen.end(), owner) != seen.end()) return own;
    seen.push_back(owner);
    Started st = *s;
    for (size_t i : indexes(fuel - 1, seen, st.introducer))
        if (i <= st.top) own.push_back(i);
    return own;
}

std::vector<KnownEntry> Rosters::known(const Key& owner) const {
    std::vector<Key> seen;
    auto is = indexes(started.size() + 1, seen, owner);
    std::sort(is.begin(), is.end());
    is.erase(std::unique(is.begin(), is.end()), is.end());
    std::vector<KnownEntry> out;
    for (size_t i : is) out.push_back({i, resolve(owner, i)});
    return out;
}

size_t Rosters::size(const Key& owner) const {
    auto k = known(owner);
    return k.empty() ? 0 : k.back().index + 1;
}

std::optional<size_t> Rosters::index_of(const Key& owner, const Key& member) const {
    for (auto& e : known(owner))
        if (e.key && *e.key == member) return e.index;
    return std::nullopt;
}

void Rosters::number(const Key& owner, size_t index, Entry e) { entries.entry(owner).entry(index, std::move(e)); }

void Rosters::retire(const Key& member, const Key& token) {
    auto renamed = [&](const Key& k) { return k == member ? token : k; };
    OMap<Key, RosterMap> ne;
    for (auto& [owner, ro] : entries) {
        RosterMap nr;
        for (auto& [i, e] : ro) {
            bool hit = (e.key && *e.key == member) ||
                       (e.prefix && !e.prefix->empty() && named(*e.prefix) == std::optional<Key>(member));
            nr.insert(i, hit ? Entry{token, std::nullopt} : e);
        }
        ne.insert(owner, nr);
    }
    if (auto ro = ne.get(member)) {
        RosterMap r = *ro;
        ne.shift_remove(member);
        ne.insert(token, r);
    }
    OMap<Key, Started> ns;
    for (auto& [o, s] : started) ns.insert(renamed(o), Started{renamed(s.introducer), s.top});
    OMap<At, Key> ni;
    for (auto& [a, k] : introductions) ni.insert(At{renamed(a.introducer), a.index}, renamed(k));
    std::vector<Key> nk;
    for (auto& k : keys) if (k != member) nk.push_back(k);
    entries = ne;
    started = ns;
    introductions = ni;
    keys = nk;
}

// ---- sent box ----

void SentBox::record(const Index& index, std::optional<Payload> plaintext, bool control, size_t epoch) {
    records.insert(counter(index), Record{index, std::move(plaintext), control, epoch, false});
}

void SentBox::retain(size_t epoch, const Retention& policy, const std::function<bool(uint64_t)>& everyone_acked) {
    if (policy.window) records.retain([&](uint64_t, const Record& r) { return epoch <= r.written + *policy.window; });
    for (auto& [k, r] : records) {
        bool expired = policy.keep_sent && !r.control && r.written + *policy.keep_sent <= epoch;
        if (expired || (policy.discard_acked && everyone_acked(k))) r.plaintext.reset();
    }
}

std::vector<Effect> SentBox::backfill(size_t epoch, const std::function<bool(uint64_t)>& everyone_acked) {
    if (backfilled == epoch) return {};
    auto ks = records.keys();
    std::sort(ks.begin(), ks.end());
    std::vector<Effect> es;
    for (uint64_t k : ks) {
        const Record& r = *records.get(k);
        bool acked = everyone_acked(k);
        if (r.plaintext && !acked) {
            Effect e(Effect::WRITE_BOX);
            e.index = r.index;
            e.payload = *r.plaintext;
            es.push_back(e);
        } else if (!r.plaintext && (!acked || !r.settled)) {
            Effect e(Effect::WRITE_TOMBSTONE);
            e.index = r.index;
            es.push_back(e);
        }
    }
    for (auto& [k, r] : records)
        if (!r.plaintext && everyone_acked(k)) r.settled = true;
    backfilled = epoch;
    return es;
}

// ---- ledger ----

std::optional<Index> Ledger::owed(const Key& stream, const std::optional<Index>& run) const {
    if (!run) return std::nullopt;
    auto p = published.get(stream);
    if (holding() || (p && *p == *run)) return std::nullopt;
    return run;
}

void Ledger::hear(const Key& stream, uint64_t idx64, const Key& by) { heard.entry(stream).insert(by, idx64); }

void Ledger::acked(const Key& member, uint64_t idx64) {
    uint64_t& v = acked_by.entry(member, idx64);
    v = std::max(v, idx64);
}

void Ledger::forget(const Key& member) {
    acked_by.shift_remove(member);
    heard.shift_remove(member);
    for (auto& [_, h] : heard) h.shift_remove(member);
}

// ---- group ----

static Key placeholder(size_t place) {
    Key k{'r', 'e', 't', 'i', 'r', 'e', 'd'};
    k.push_back((uint8_t)place);
    return k;
}

Group Group::make(const Bytes& read_cap, Retention policy, size_t recheck) {
    Group g;
    g.read_cap = read_cap;
    g.policy = policy;
    g.recheck = recheck;
    Index start = start_of(read_cap);
    g.pool = {start};
    g.tip = start;
    return g;
}

void Group::follow(const Bytes& rc, Index start) { readers.insert(key_of(rc), Reader::make(rc, std::move(start))); }

void Group::follow_if_new(const Bytes& rc) {
    if (!readers.contains(key_of(rc))) follow(rc, start_of(rc));
}

Group Group::found(const Bytes& read_cap, const std::vector<Bytes>& founders, Retention policy, size_t recheck) {
    Group g = make(read_cap, policy, recheck);
    std::vector<Key> ks;
    for (auto& c : founders) ks.push_back(key_of(c));
    g.rosters.found(ks);
    for (auto& c : founders)
        if (key_of(c) != g.me()) g.follow(c, start_of(c));
    return g;
}

Group Group::joined(const Bytes& read_cap, const JoinReply& reply, Retention policy, size_t recheck) {
    Group g = make(read_cap, policy, recheck);
    Key me = g.me();
    std::vector<Key> order;
    for (size_t place = 0; place < reply.members.size(); place++)
        order.push_back(reply.members[place] ? key_of(reply.members[place]->read_cap) : placeholder(place));
    order.push_back(me);
    OMap<Key, RosterMap> entries;
    for (size_t place = 0; place < reply.rosters.size(); place++) {
        if (!reply.rosters[place] || place >= order.size()) continue;
        RosterMap r;
        for (auto& e : *reply.rosters[place]) {
            std::optional<Key> k;
            if (e.place && *e.place < order.size()) k = order[*e.place];
            r.insert(e.index, Entry{k, std::nullopt});
        }
        entries.insert(order[place], r);
    }
    for (auto& i : reply.introductions)
        if (i.by < order.size() && i.member < order.size())
            g.rosters.introductions.insert(At{order[i.by], i.index}, order[i.member]);
    for (auto& m : reply.members)
        if (m) g.rosters.learn(key_of(m->read_cap));
    g.rosters.learn(me);
    Key introducer = reply.introducer < order.size() ? order[reply.introducer] : Key{};
    RosterMap own;
    if (auto r = entries.get(introducer)) own = *r;
    own.insert(reply.newcomer, Entry{me, std::nullopt});
    entries.insert(me, own);
    g.rosters.entries = entries;
    for (auto& m : reply.members)
        if (m) g.follow(m->read_cap, m->start);
    g.joined_through = introducer;
    return g;
}

std::vector<uint64_t> Group::heard_of(const Key& stream) const {
    std::vector<uint64_t> out;
    if (auto h = ledger.heard.get(stream))
        for (auto& p : *h) out.push_back(p.second);
    return out;
}

std::vector<Effect> Group::scan_stream(const Key& stream) {
    auto heard = heard_of(stream);
    auto r = readers.get(stream);
    return r ? r->scan(heard, recheck) : std::vector<Effect>{};
}

bool Group::numbered(const Key& member) const {
    if (rosters.index_of(me(), member)) return true;
    for (auto& [_, p] : pending) {
        if (p.introduced && *p.introduced == member) return true;
        for (auto& n : p.numbers)
            if (n.at) {
                auto k = rosters.introductions.get(*n.at);
                if (k && *k == member) return true;
            }
    }
    return false;
}

void Group::begin_roster(const Key& member, const Key& introducer) {
    if (auto place = rosters.index_of(introducer, member)) rosters.started.insert(member, Started{introducer, *place});
}

void Group::started_at(const Key& member, const Index& index) {
    std::optional<Key> found;
    for (auto& [a, n] : rosters.introductions)
        if (n == member && a.index == index) { found = a.introducer; break; }
    if (found) begin_roster(member, *found);
    else awaiting_start.insert(member, index);
}

void Group::note_own(const Key& introducer, const Index& index, const Key& added) {
    if (added != me() || !joined_through || *joined_through != introducer) return;
    if (auto place = rosters.index_of(me(), introducer)) {
        introduced_at = IntroRef{*place, index};
        joined_through.reset();
    }
}

void Group::note_awaited(const Key& introducer, const Index& index, const Key& added) {
    auto w = awaiting_start.get(added);
    if (w && *w == index) {
        awaiting_start.shift_remove(added);
        begin_roster(added, introducer);
    }
}

void Group::note_new(const Key& introducer, const Index& index, const Introduction& intro) {
    Key added = key_of(intro.read_cap);
    if (added == me() || numbered(added)) return;
    follow_if_new(intro.read_cap);
    At a{introducer, index};
    if (std::find(unnumbered.begin(), unnumbered.end(), a) == unnumbered.end()) unnumbered.push_back(a);
}

void Group::introduced(const Key& introducer, const Index& index, const Introduction& intro) {
    Key added = key_of(intro.read_cap);
    rosters.introductions.insert(At{introducer, index}, added);
    rosters.learn(added);
    note_own(introducer, index, added);
    note_awaited(introducer, index, added);
    note_new(introducer, index, intro);
}

void Group::take_ack(const Key& sender, const Ack& a) {
    auto acked = rosters.resolve(sender, a.member);
    if (!acked) return;
    if (*acked == me()) {
        if (sent.check_at(a.idx64)) ledger.acked(sender, a.idx64);
    } else {
        ledger.hear(*acked, a.idx64, sender);
    }
}

std::vector<Effect> Group::deliver(const Key& sender, const Index& index, const Message& m) {
    if (!m.content) return {};
    if (policy.keep_received) received.insert(ReceivedAt{sender, index}, epoch);
    Effect e(Effect::DELIVER);
    e.sender = sender;
    e.index = index;
    e.content = *m.content;
    return {e};
}

std::vector<Effect> Group::ingest(const Key& sender, const Index& index, const Message& m) {
    for (auto& n : m.numbers) {
        Entry e;
        if (n.prefix) e.prefix = *n.prefix;
        else if (m.introduction) e.key = key_of(m.introduction->read_cap);
        rosters.number(sender, n.index, e);
    }
    if (m.introduction) introduced(sender, index, *m.introduction);
    if (m.introduced_at) started_at(sender, m.introduced_at->index);
    for (auto& a : m.acks) take_ack(sender, a);
    return deliver(sender, index, m);
}

std::vector<Effect> Group::box_read(const Key& stream, const Index& index, const ReadResult& result,
                                   const std::optional<Index>& next) {
    auto r = readers.get(stream);
    if (!r) return {};
    auto [m, es] = r->found(index, result, next, recheck);
    if (!m) return es;
    auto out = ingest(stream, index, *m);
    out.insert(out.end(), es.begin(), es.end());
    return out;
}

size_t Group::size() const {
    size_t m = rosters.size(me());
    for (auto& [_, p] : pending)
        for (auto& n : p.numbers) m = std::max(m, n.index + 1);
    return m;
}

bool Group::room() const { return size() < ROSTER_MAX; }

Bytes Group::membership(const Key* leaving) const {
    std::vector<Bytes> caps;
    if (!leaving || key_of(read_cap) != *leaving) caps.push_back(read_cap);
    for (auto& [_, r] : readers)
        if (!leaving || key_of(r.read_cap) != *leaving) caps.push_back(r.read_cap);
    auto h = membership_hash(caps);
    return Bytes(h.begin(), h.end());
}

bool Group::owes_control() const { return !ledger.holding() && (introduced_at || !numberable().empty()); }

std::vector<std::pair<At, Key>> Group::numberable() const {
    std::vector<std::pair<At, Key>> out;
    size_t sz = size();
    for (auto& a : unnumbered) {
        auto added = rosters.introductions.get(a);
        if (!added) continue;
        Bytes p = prefix_of(*added);
        bool dup = false;
        for (auto& o : out) dup |= prefix_of(o.second) == p;
        if (sz + out.size() < ROSTER_MAX && p.size() == PREFIX_SIZE && !dup) out.push_back({a, *added});
    }
    return out;
}

std::pair<Message, Pending> Group::control() {
    Message m;
    m.introduced_at = introduced_at;
    m.membership_hash = membership(nullptr);
    Pending p;
    p.control = true;
    auto left = unnumbered;
    size_t next = size();
    for (auto& [a, added] : numberable()) {
        m.numbers.push_back({next, prefix_of(added)});
        p.numbers.push_back({next, a});
        auto it = std::find(left.begin(), left.end(), a);
        if (it != left.end()) left.erase(it);
        next++;
    }
    unnumbered = left;
    introduced_at.reset();
    return {m, p};
}

void Group::release() {
    ledger.induction = Induction::IDLE;
    ledger.invitee.reset();
    ledger.commit = false;
    ledger.starts.clear();
}

void Group::hold() {
    ledger.induction = Induction::INDUCTING;
    ledger.starts.clear();
    for (auto& [k, r] : readers) ledger.starts.insert(k, r.next);
}

std::pair<Message, Pending> Group::introduction_of(const std::string& name, const Bytes& rc) const {
    Key added = key_of(rc);
    Message m;
    m.introduction = Introduction{name, rc};
    m.membership_hash = membership(&added);
    Pending p;
    p.introduced = added;
    p.control = true;
    if (!numbered(added) && room()) {
        size_t n = size();
        m.numbers = {{n, std::nullopt}};
        p.numbers = {{n, std::nullopt}};
    }
    return {m, p};
}

std::pair<Message, Pending> Group::introduction(const std::string& name, const Bytes& rc) {
    auto out = introduction_of(name, rc);
    follow_if_new(rc);
    release();
    return out;
}

void Group::with_acks(Message& m, Pending& p) {
    for (auto& e : rosters.known(me())) {
        if (!e.key) continue;
        auto r = readers.get(*e.key);
        if (!r) continue;
        auto owed = ledger.owed(r->stream, r->run());
        if (!owed) continue;
        m.acks.push_back({e.index, counter(*owed)});
        Key stream = r->stream;
        auto before = ledger.published.get(stream);
        p.acks.insert(stream, AckedRun{before ? std::optional<Index>(*before) : std::nullopt, *owed});
        ledger.published.insert(stream, *owed);
    }
}

Outgoing Group::stage(Message m, Pending p) {
    tokens += 1;
    pending.insert(tokens, std::move(p));
    return Outgoing{tokens, std::move(m)};
}

void Group::written(size_t token, const Index& index, std::optional<Payload> plaintext) {
    allocates = false;
    landed(token, index, std::move(plaintext));
}

void Group::landed(size_t token, const Index& index, std::optional<Payload> plaintext) {
    auto pp = pending.get(token);
    if (!pp) return;
    Pending p = *pp;
    Key m = me();
    for (auto& n : p.numbers) {
        std::optional<Key> k;
        if (n.at) {
            if (auto x = rosters.introductions.get(*n.at)) k = *x;
        } else {
            k = p.introduced;
        }
        rosters.number(m, n.index, Entry{k, std::nullopt});
    }
    if (p.introduced) {
        rosters.learn(*p.introduced);
        rosters.introductions.insert(At{m, index}, *p.introduced);
    }
    pending.shift_remove(token);
    sent.record(index, std::move(plaintext), p.control, epoch);
}

std::vector<Effect> Group::write(Outgoing o) {
    if (pool.empty()) return {};
    Index index = pool.front();
    pool.erase(pool.begin());
    landed(o.token, index, Payload::of(o.message));
    Effect e(Effect::WRITE_BOX);
    e.index = index;
    e.payload = Payload::of(std::move(o.message));
    return {e};
}

std::pair<Message, Pending> Group::next_message() {
    if (owes_control()) return control();
    Message m;
    if (!outbox.empty()) {
        m.content = outbox.front();
        outbox.erase(outbox.begin());
    }
    return {m, Pending{}};
}

std::vector<Effect> Group::flush() {
    std::vector<Effect> es;
    for (size_t i = 0, n = pool.size(); i < n; i++) {
        if (!allocates || pool.empty() || !(owes_control() || !outbox.empty())) break;
        auto [m, p] = next_message();
        with_acks(m, p);
        auto w = write(stage(std::move(m), std::move(p)));
        es.insert(es.end(), w.begin(), w.end());
    }
    if (allocates && pool.size() < 2 && !deriving) {
        deriving = true;
        Effect e(Effect::DERIVE_INDEXES);
        e.stream = me();
        e.start = tip;
        e.count = POOL_SIZE;
        es.push_back(e);
    }
    return es;
}

bool Group::settled() const {
    if (!unnumbered.empty() || !outbox.empty()) return false;
    for (auto& [_, r] : readers)
        if (r.mode != Mode::IDLE) return false;
    return true;
}

JoinReply Group::reply(const Index& own_start) const {
    auto order = rosters.known(me());
    size_t sz = order.empty() ? 0 : order.back().index + 1;
    OMap<Key, size_t> place;
    for (auto& e : order)
        if (e.key) place.insert(*e.key, e.index);
    auto key_at = [&](size_t index) -> std::optional<Key> {
        for (auto& e : order)
            if (e.index == index) return e.key;
        return std::nullopt;
    };
    Key m = me();
    JoinReply j;
    for (size_t index = 0; index < sz; index++) {
        auto k = key_at(index);
        if (!k) { j.members.push_back(std::nullopt); continue; }
        if (*k == m) { j.members.push_back(Member{read_cap, own_start}); continue; }
        auto r = readers.get(*k);
        if (!r) { j.members.push_back(std::nullopt); continue; }
        auto s = ledger.starts.get(*k);
        j.members.push_back(Member{r->read_cap, s ? *s : r->next});
    }
    for (size_t index = 0; index < sz; index++) {
        auto k = key_at(index);
        auto known = k ? rosters.known(*k) : std::vector<KnownEntry>{};
        if (known.empty()) { j.rosters.push_back(std::nullopt); continue; }
        std::vector<RosterEntry> wr;
        for (auto& e : known) {
            std::optional<size_t> pl;
            if (e.key)
                if (auto x = place.get(*e.key)) pl = *x;
            wr.push_back({e.index, pl});
        }
        j.rosters.push_back(wr);
    }
    for (auto& [a, k] : rosters.introductions) {
        auto by = place.get(a.introducer), mem = place.get(k);
        if (by && mem) j.introductions.push_back({*by, a.index, *mem});
    }
    auto mine = place.get(m);
    j.introducer = mine ? *mine : 0;
    j.newcomer = sz;
    return j;
}

Outgoing Group::introduce_now(const std::string& name, const Bytes& rc) {
    allocates = false;
    auto [m, p] = introduction(name, rc);
    return stage(std::move(m), std::move(p));
}

std::vector<Effect> Group::commit_induction() {
    if (!ledger.invitee) return {};
    Invitee inv = *ledger.invitee;
    Index head = pool.empty() ? Index{} : pool.front();
    JoinReply rep = reply(head);
    auto [m, p] = introduction(inv.name, inv.read_cap);
    auto es = write(stage(std::move(m), std::move(p)));
    Effect e(Effect::SEND_JOIN_REPLY);
    e.to = inv.read_cap;
    e.reply = rep;
    es.push_back(e);
    auto f = flush();
    es.insert(es.end(), f.begin(), f.end());
    return es;
}

std::vector<Effect> Group::progress() {
    auto es = flush();
    if (ledger.induction == Induction::CATCHING_UP && settled()) hold();
    if (ledger.induction == Induction::INDUCTING && ledger.commit && allocates && !pool.empty()) {
        auto c = commit_induction();
        es.insert(es.end(), c.begin(), c.end());
    }
    return es;
}

std::vector<Effect> Group::derived(const std::vector<Index>& indexes) {
    deriving = false;
    if (!indexes.empty()) tip = indexes.back();
    pool.insert(pool.end(), indexes.begin(), indexes.end());
    return progress();
}

std::vector<Effect> Group::invite(const std::string& name, const Bytes& rc, bool commit) {
    ledger.induction = Induction::CATCHING_UP;
    ledger.invitee = Invitee{name, rc};
    ledger.commit = commit;
    std::vector<Effect> es;
    for (auto& [_, r] : readers) {
        auto x = r.read();
        es.insert(es.end(), x.begin(), x.end());
    }
    auto p = progress();
    es.insert(es.end(), p.begin(), p.end());
    return es;
}

void Group::remove(const Key& member) {
    Key token;
    if (auto place = rosters.index_of(me(), member)) {
        token = placeholder(*place);
    } else {
        token = {'r', 'e', 't', 'i', 'r', 'e', 'd', ':'};
        token.insert(token.end(), member.begin(), member.begin() + std::min<size_t>(8, member.size()));
    }
    rosters.retire(member, token);
    readers.shift_remove(member);
    ledger.forget(member);
    unnumbered.erase(std::remove_if(unnumbered.begin(), unnumbered.end(),
                                    [&](const At& a) {
                                        auto k = rosters.introductions.get(a);
                                        return k && *k == token;
                                    }),
                     unnumbered.end());
}

bool Group::everyone_acked(uint64_t idx64) const {
    for (auto& [k, _] : readers) {
        auto s = ledger.acked_by.get(k);
        if (!s || idx64 > *s) return false;
    }
    return true;
}

std::vector<Effect> Group::on_epoch(size_t e) {
    epoch = e;
    auto acked = [this](uint64_t k) { return everyone_acked(k); };
    sent.retain(e, policy, acked);
    std::vector<Effect> es;
    if (!policy.keep_received) received.clear();
    if (policy.keep_received) {
        std::vector<ReceivedAt> expired;
        for (auto& [k, at] : received)
            if (at + *policy.keep_received <= e) expired.push_back(k);
        for (auto& k : expired) {
            Effect x(Effect::EXPIRE);
            x.sender = k.sender;
            x.index = k.index;
            es.push_back(x);
        }
        received.retain([&](const ReceivedAt& k, size_t) {
            return std::find(expired.begin(), expired.end(), k) == expired.end();
        });
    }
    auto b = sent.backfill(e, acked);
    es.insert(es.end(), b.begin(), b.end());
    return es;
}

std::vector<Effect> Group::handle(const Event& e) {
    switch (e.kind) {
        case Event::SEND:
            outbox.push_back(e.content);
            return progress();
        case Event::READ: {
            auto r = readers.get(e.stream);
            return r ? r->read() : std::vector<Effect>{};
        }
        case Event::SCAN:
            return scan_stream(e.stream);
        case Event::BOX_READ: {
            auto es = box_read(e.stream, e.index, e.result, e.next);
            auto p = progress();
            es.insert(es.end(), p.begin(), p.end());
            return es;
        }
        case Event::INDEXES_DERIVED:
            return derived(e.indexes);
        case Event::WALKED: {
            auto r = readers.get(e.stream);
            return r ? r->walked(e.walked, recheck) : std::vector<Effect>{};
        }
        case Event::INVITE:
            return invite(e.name, e.read_cap, false);
        case Event::INTRODUCE:
            if (ledger.induction == Induction::IDLE) return invite(e.name, e.read_cap, true);
            ledger.commit = true;
            return progress();
        case Event::REMOVE:
            remove(e.member);
            return {};
        case Event::EPOCH_CHANGED:
            return on_epoch(e.epoch);
    }
    return {};
}

std::vector<Effect> Group::observe(const Key& stream, const Index& index, std::optional<Message> m, const Index& next) {
    auto r = readers.get(stream);
    if (!r) return {};
    r->next = index;
    r->mode = Mode::READING;
    ReadResult res{ReadResult::DATA, std::move(m)};
    auto es = box_read(stream, index, res, next);
    if (auto r2 = readers.get(stream)) r2->mode = Mode::IDLE;
    std::vector<Effect> out;
    for (auto& x : es)
        if (x.kind == Effect::DELIVER || x.kind == Effect::EXPIRE) out.push_back(x);
    return out;
}

std::vector<Outgoing> Group::compose(std::optional<Bytes> content) {
    allocates = false;
    std::vector<std::pair<Message, Pending>> owed;
    if (owes_control()) owed.push_back(control());
    if (content) {
        Message m;
        m.content = std::move(content);
        owed.push_back({m, Pending{}});
    }
    std::vector<Outgoing> out;
    for (auto& [m, p] : owed) {
        with_acks(m, p);
        out.push_back(stage(m, p));
    }
    return out;
}

void Group::abandoned(size_t token) {
    auto pp = pending.get(token);
    if (!pp) return;
    Pending p = *pp;
    pending.shift_remove(token);
    for (auto& [stream, a] : p.acks) {
        auto cur = ledger.published.get(stream);
        if (cur && *cur == a.now) {
            if (a.before) ledger.published.insert(stream, *a.before);
            else ledger.published.shift_remove(stream);
        }
    }
    if (!p.introduced)
        for (auto& n : p.numbers)
            if (n.at) unnumbered.push_back(*n.at);
}

std::vector<Outgoing> Group::begin_induction() {
    auto out = compose(std::nullopt);
    hold();
    return out;
}

}  // namespace kq1
