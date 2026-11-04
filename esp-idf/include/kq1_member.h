// SPDX-License-Identifier: AGPL-3.0-only
// The katzenqt1 member, sans IO, as pigeonhole_protocols rust/src/protocols/katzenqt1/member: one
// group as one member sees it. Group::handle takes an Event and returns the Effects to carry out.
#pragma once
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kq1_wire.h"

namespace kq1 {

using Key = Bytes;
using Index = Bytes;

// An insertion-ordered map, as Rust's IndexMap: maps here hold a group's members, so linear search.
template <class K, class V>
struct OMap {
    std::vector<std::pair<K, V>> items;

    auto begin() { return items.begin(); }
    auto end() { return items.end(); }
    auto begin() const { return items.begin(); }
    auto end() const { return items.end(); }
    size_t size() const { return items.size(); }
    bool empty() const { return items.empty(); }
    void clear() { items.clear(); }
    V* get(const K& k) {
        for (auto& p : items) if (p.first == k) return &p.second;
        return nullptr;
    }
    const V* get(const K& k) const {
        for (auto& p : items) if (p.first == k) return &p.second;
        return nullptr;
    }
    bool contains(const K& k) const { return get(k) != nullptr; }
    // Replaces the value in place when k is there, else appends.
    V& insert(const K& k, V v) {
        if (V* x = get(k)) { *x = std::move(v); return *x; }
        items.emplace_back(k, std::move(v));
        return items.back().second;
    }
    V& entry(const K& k, V init = V()) {
        if (V* x = get(k)) return *x;
        items.emplace_back(k, std::move(init));
        return items.back().second;
    }
    bool shift_remove(const K& k) {
        for (auto it = items.begin(); it != items.end(); ++it)
            if (it->first == k) { items.erase(it); return true; }
        return false;
    }
    template <class P>
    void retain(P keep) {
        items.erase(std::remove_if(items.begin(), items.end(), [&](auto& p) { return !keep(p.first, p.second); }),
                    items.end());
    }
    std::vector<K> keys() const {
        std::vector<K> ks;
        for (auto& p : items) ks.push_back(p.first);
        return ks;
    }
    bool operator==(const OMap& o) const { return items == o.items; }
};

bool operator==(const Message& a, const Message& b);

std::optional<Index> next_index(const Index& i);
constexpr uint64_t WALK_LIMIT = 256;
// The Walk effect carried out: the target index, the indexes passed over that are kept, the steps.
struct WalkResult {
    std::optional<Index> target;
    std::vector<Index> skipped;
    size_t steps = 0;
};
std::optional<WalkResult> walk_with(const Index& start, const std::vector<uint64_t>& targets, size_t keep);
std::optional<std::vector<Index>> derive_with(const Index& start, size_t count);

struct At {
    Key introducer;
    Index index;
    bool operator==(const At& o) const { return introducer == o.introducer && index == o.index; }
};

struct ReadResult {
    enum Kind { DATA, TOMBSTONE, NOT_FOUND } kind = NOT_FOUND;
    std::optional<Message> message;  // DATA: None when the box cannot be read as a message
    bool operator==(const ReadResult& o) const { return kind == o.kind && message == o.message; }
};

struct Payload {
    bool is_bytes = false;
    Message message;
    Bytes bytes;
    static Payload of(Message m) { Payload p; p.message = std::move(m); return p; }
    static Payload raw(Bytes b) { Payload p; p.is_bytes = true; p.bytes = std::move(b); return p; }
    bool operator==(const Payload& o) const {
        return is_bytes == o.is_bytes && (is_bytes ? bytes == o.bytes : message == o.message);
    }
};

struct Event {
    enum Kind { SEND, READ, SCAN, BOX_READ, INDEXES_DERIVED, WALKED, INVITE, INTRODUCE, REMOVE, EPOCH_CHANGED } kind;
    explicit Event(Kind k) : kind(k) {}
    Bytes content;              // SEND
    Key stream;                 // READ, SCAN, BOX_READ, INDEXES_DERIVED, WALKED
    Index index;                // BOX_READ
    ReadResult result;          // BOX_READ
    std::optional<Index> next;  // BOX_READ
    std::vector<Index> indexes; // INDEXES_DERIVED
    WalkResult walked;          // WALKED
    std::string name;           // INVITE, INTRODUCE
    Bytes read_cap;             // INVITE, INTRODUCE
    Key member;                 // REMOVE
    size_t epoch = 0;           // EPOCH_CHANGED
};

struct Effect {
    enum Kind { WRITE_BOX, WRITE_TOMBSTONE, READ_BOX, DERIVE_INDEXES, WALK, DELIVER, EXPIRE, SEND_JOIN_REPLY } kind;
    explicit Effect(Kind k) : kind(k) {}
    Index index;                    // WRITE_BOX, WRITE_TOMBSTONE, READ_BOX, DELIVER, EXPIRE
    Payload payload;                // WRITE_BOX
    Key stream;                     // READ_BOX, DERIVE_INDEXES, WALK
    Bytes read_cap;                 // READ_BOX
    bool recheck = false;           // READ_BOX
    Index start;                    // DERIVE_INDEXES, WALK
    size_t count = 0;               // DERIVE_INDEXES
    std::vector<uint64_t> targets;  // WALK
    size_t keep = 0;                // WALK
    Key sender;                     // DELIVER, EXPIRE
    Bytes content;                  // DELIVER
    Bytes to;                       // SEND_JOIN_REPLY: the newcomer's read cap
    JoinReply reply;                // SEND_JOIN_REPLY
};

// ---- reader ----

enum class Mode { IDLE, READING, SCANNING };
struct Kept { Index index; size_t passed; std::optional<Index> before; bool operator==(const Kept& o) const { return index == o.index && passed == o.passed && before == o.before; } };
struct Lost { size_t passed; std::optional<Index> before; };

struct Reader {
    Key stream;
    Bytes read_cap;
    Index next;
    std::optional<Index> last;
    size_t passed = 0;
    std::vector<Kept> kept;
    std::optional<Lost> lost;
    Mode mode = Mode::IDLE;
    std::vector<uint64_t> tried;

    static Reader make(const Bytes& read_cap, Index next);
    std::vector<Effect> read();
    std::vector<Effect> scan(const std::vector<uint64_t>& heard, size_t window);
    // The index this reader has read everything up to, which acks may name.
    std::optional<Index> run() const;
    std::vector<Effect> walked(const WalkResult& w, size_t window);
    std::pair<std::optional<Message>, std::vector<Effect>> found(const Index& index, const ReadResult& result,
                                                                 const std::optional<Index>& next, size_t window);

private:
    Effect read_box(const Index& index, bool recheck) const;
    std::vector<uint64_t> ahead(const std::vector<uint64_t>& heard) const;
    void lose(size_t at, std::optional<Index> before);
    void forget(size_t window);
    void jump(const Index& t, const std::vector<Index>& skipped, size_t steps);
};

// ---- rosters ----

struct Entry { std::optional<Key> key, prefix; bool operator==(const Entry& o) const { return key == o.key && prefix == o.prefix; } };
struct Started { Key introducer; size_t top; bool operator==(const Started& o) const { return introducer == o.introducer && top == o.top; } };
using RosterMap = OMap<size_t, Entry>;
struct KnownEntry { size_t index; std::optional<Key> key; };

struct Rosters {
    OMap<Key, RosterMap> entries;
    OMap<Key, Started> started;
    OMap<At, Key> introductions;
    std::vector<Key> keys;

    void learn(const Key& k);
    void found(const std::vector<Key>& founders);
    std::optional<Key> resolve(const Key& owner, size_t index) const;
    std::vector<KnownEntry> known(const Key& owner) const;
    size_t size(const Key& owner) const;
    std::optional<size_t> index_of(const Key& owner, const Key& member) const;
    void number(const Key& owner, size_t index, Entry e);
    void retire(const Key& member, const Key& token);

private:
    std::optional<Key> named(const Bytes& prefix) const;
    std::vector<size_t> indexes(size_t fuel, std::vector<Key>& seen, const Key& owner) const;
};

// ---- sent box ----

struct Retention {
    std::optional<size_t> window = 3;
    bool discard_acked = true;
    std::optional<size_t> keep_sent, keep_received;
};

struct Record {
    Index index;
    std::optional<Payload> plaintext;
    bool control = false;
    size_t written = 0;
    bool settled = false;
};

struct SentBox {
    OMap<uint64_t, Record> records;
    std::optional<size_t> backfilled;

    void record(const Index& index, std::optional<Payload> plaintext, bool control, size_t epoch);
    bool check_at(uint64_t idx64) const { return records.contains(idx64); }
    void retain(size_t epoch, const Retention& policy, const std::function<bool(uint64_t)>& everyone_acked);
    std::vector<Effect> backfill(size_t epoch, const std::function<bool(uint64_t)>& everyone_acked);
};

// ---- ledger ----

enum class Induction { IDLE, CATCHING_UP, INDUCTING };
struct Invitee { std::string name; Bytes read_cap; };

struct Ledger {
    OMap<Key, Index> published;
    OMap<Key, uint64_t> acked_by;
    OMap<Key, OMap<Key, uint64_t>> heard;
    Induction induction = Induction::IDLE;
    std::optional<Invitee> invitee;
    bool commit = false;
    OMap<Key, Index> starts;

    bool holding() const { return induction == Induction::INDUCTING; }
    std::optional<Index> owed(const Key& stream, const std::optional<Index>& run) const;
    void hear(const Key& stream, uint64_t idx64, const Key& by);
    void acked(const Key& member, uint64_t idx64);
    void forget(const Key& member);
};

// ---- group ----

constexpr size_t POOL_SIZE = 4;

struct NumberAt { size_t index; std::optional<At> at; };
struct AckedRun { std::optional<Index> before; Index now; };
struct Pending {
    std::vector<NumberAt> numbers;
    std::optional<Key> introduced;
    OMap<Key, AckedRun> acks;
    bool control = false;
};
struct Outgoing { size_t token; Message message; };
struct ReceivedAt { Key sender; Index index; bool operator==(const ReceivedAt& o) const { return sender == o.sender && index == o.index; } };

struct Group {
    Bytes read_cap;
    Retention policy;
    size_t recheck = 256;
    Rosters rosters;
    OMap<Key, Reader> readers;
    SentBox sent;
    Ledger ledger;
    size_t epoch = 0;
    std::vector<Index> pool;
    Index tip;
    bool deriving = false;
    std::vector<Bytes> outbox;
    OMap<size_t, Pending> pending;
    size_t tokens = 0;
    std::vector<At> unnumbered;
    std::optional<IntroRef> introduced_at;
    std::optional<Key> joined_through;
    OMap<Key, Index> awaiting_start;
    bool allocates = true;
    OMap<ReceivedAt, size_t> received;

    static Group found(const Bytes& read_cap, const std::vector<Bytes>& founders, Retention policy, size_t recheck);
    static Group joined(const Bytes& read_cap, const JoinReply& reply, Retention policy, size_t recheck);

    Key me() const { return key_of(read_cap); }
    std::vector<Effect> handle(const Event& e);
    // A box read outside the member's own reads (the client reads streams itself): only
    // Deliver and Expire come back.
    std::vector<Effect> observe(const Key& stream, const Index& index, std::optional<Message> m, const Index& next);
    // The messages owed now, each with a token for written() or abandoned().
    std::vector<Outgoing> compose(std::optional<Bytes> content);
    void written(size_t token, const Index& index, std::optional<Payload> plaintext);
    void abandoned(size_t token);
    std::vector<Outgoing> begin_induction();
    void cancel_induction() { release(); }
    Outgoing introduce_now(const std::string& name, const Bytes& read_cap);
    JoinReply reply(const Index& own_start) const;
    void remove(const Key& member);
    bool room() const;
    Bytes membership(const Key* leaving) const;
    std::optional<Key> resolve(const Key& owner, size_t index) const { return rosters.resolve(owner, index); }

private:
    static Group make(const Bytes& read_cap, Retention policy, size_t recheck);
    void follow(const Bytes& read_cap, Index start);
    void follow_if_new(const Bytes& read_cap);
    std::vector<uint64_t> heard_of(const Key& stream) const;
    std::vector<Effect> scan_stream(const Key& stream);
    bool numbered(const Key& member) const;
    void begin_roster(const Key& member, const Key& introducer);
    void started_at(const Key& member, const Index& index);
    void note_own(const Key& introducer, const Index& index, const Key& added);
    void note_awaited(const Key& introducer, const Index& index, const Key& added);
    void note_new(const Key& introducer, const Index& index, const Introduction& intro);
    void introduced(const Key& introducer, const Index& index, const Introduction& intro);
    void take_ack(const Key& sender, const Ack& a);
    std::vector<Effect> deliver(const Key& sender, const Index& index, const Message& m);
    std::vector<Effect> ingest(const Key& sender, const Index& index, const Message& m);
    std::vector<Effect> box_read(const Key& stream, const Index& index, const ReadResult& result,
                                 const std::optional<Index>& next);
    size_t size() const;
    bool owes_control() const;
    std::vector<std::pair<At, Key>> numberable() const;
    std::pair<Message, Pending> control();
    void release();
    void hold();
    std::pair<Message, Pending> introduction_of(const std::string& name, const Bytes& read_cap) const;
    std::pair<Message, Pending> introduction(const std::string& name, const Bytes& read_cap);
    void with_acks(Message& m, Pending& p);
    Outgoing stage(Message m, Pending p);
    void landed(size_t token, const Index& index, std::optional<Payload> plaintext);
    std::vector<Effect> write(Outgoing o);
    std::pair<Message, Pending> next_message();
    std::vector<Effect> flush();
    bool settled() const;
    std::vector<Effect> commit_induction();
    std::vector<Effect> progress();
    std::vector<Effect> derived(const std::vector<Index>& indexes);
    std::vector<Effect> invite(const std::string& name, const Bytes& read_cap, bool commit);
    bool everyone_acked(uint64_t idx64) const;
    std::vector<Effect> on_epoch(size_t epoch);
};

}  // namespace kq1
