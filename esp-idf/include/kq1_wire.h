// SPDX-License-Identifier: AGPL-3.0-only
// The katzenqt1 wire, version 4 arrays and version 3 maps, as pigeonhole_protocols
// rust/src/protocols/katzenqt1/wire.
#pragma once
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kq_crypto.h"

namespace kq1 {

using kq::Bytes;

constexpr size_t KEY_SIZE = 32, INDEX_SIZE = 104, READ_CAP_SIZE = 136, HASH_SIZE = 32, ROSTER_MAX = 256,
                 PREFIX_SIZE = 8, MAX_NAME_CHARS = 30;
constexpr uint64_t CURRENT_VERSION = 4, MAP_VERSION = 3;
constexpr uint64_t MSG_TEXT = 0, MSG_INTRODUCTION = 1, MSG_FILE_UPLOAD = 2, MSG_WHO = 3, MSG_REPLY_WHO = 4,
                   MSG_CONTROL = 10;

// ---- CBOR, byte level ----

struct Span {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool empty() const { return n == 0; }
    void skip(size_t k) { p += k; n -= k; }
};
inline Span span(const Bytes& b) { return {b.data(), b.size()}; }

void push_header(Bytes& out, uint8_t major, uint64_t n);
inline void push_uint(Bytes& out, uint64_t n) { push_header(out, 0, n); }
void push_bytes(Bytes& out, const uint8_t* p, size_t n);
inline void push_bytes(Bytes& out, const Bytes& b) { push_bytes(out, b.data(), b.size()); }
void push_text(Bytes& out, const std::string& s);
inline void push_null(Bytes& out) { out.push_back(0xf6); }

bool read_header(Span& l, uint8_t& major, uint64_t& n);
bool read_uint(Span& l, uint64_t& n);
bool read_bytes(Span& l, Bytes& out);
bool read_text(Span& l, std::string& out);
bool read_array(Span& l, uint64_t& n);
inline bool is_null(Span l) { return l.n && l.p[0] == 0xf6; }
bool skip_items(size_t fuel, uint64_t count, Span& l);
bool read_raw(Span& l, Bytes& out);
size_t utf8_chars(const std::string& s);

// A decoded CBOR value, as the Rust Term: integers, strings, lists, maps; tags and others opaque.
struct Term {
    enum Kind { INT, BYTES, TEXT, LIST, MAP, TAG, NUL, OTHER } kind = NUL;
    bool negative = false;
    uint64_t value = 0;
    Bytes bytes;  // BYTES and TEXT
    std::vector<Term> items;  // LIST; MAP as key, value, key, value
    static Term nat(uint64_t n) { Term t; t.kind = INT; t.value = n; return t; }
    static Term text(const std::string& s) { Term t; t.kind = TEXT; t.bytes.assign(s.begin(), s.end()); return t; }
    static Term blob(const Bytes& b) { Term t; t.kind = BYTES; t.bytes = b; return t; }
    static Term list(std::vector<Term> v) { Term t; t.kind = LIST; t.items = std::move(v); return t; }
    bool is_null() const { return kind == NUL; }
    std::optional<size_t> as_nat() const;
    std::string str() const { return std::string(bytes.begin(), bytes.end()); }
    const Term* field(const std::string& key) const;  // the last value under a text key
    void write(Bytes& out) const;
    static std::optional<Term> read(const uint8_t* p, size_t n);
};

// ---- fields ----

struct Ack { size_t member; uint64_t idx64; bool operator==(const Ack& o) const { return member == o.member && idx64 == o.idx64; } };
struct Number { size_t index; std::optional<Bytes> prefix; bool operator==(const Number& o) const { return index == o.index && prefix == o.prefix; } };
struct IntroRef { size_t introducer; Bytes index; bool operator==(const IntroRef& o) const { return introducer == o.introducer && index == o.index; } };
struct Introduction { std::string name; Bytes read_cap; bool operator==(const Introduction& o) const { return name == o.name && read_cap == o.read_cap; } };

Bytes acks_to_bytes(const std::vector<Ack>& acks);
// nullopt on a malformed field.
std::optional<std::vector<Ack>> acks_from_bytes(const Bytes& field);
Bytes numbers_to_bytes(const std::vector<Number>& numbers);
std::vector<Number> numbers_from_bytes(const Bytes& field);
Bytes introduced_at_to_bytes(const IntroRef& r);
std::optional<IntroRef> introduced_at_from_bytes(const Bytes& field);

inline Bytes key_of(const Bytes& read_cap) { return Bytes(read_cap.begin(), read_cap.begin() + std::min(KEY_SIZE, read_cap.size())); }
inline Bytes start_of(const Bytes& read_cap) { return Bytes(read_cap.begin() + std::min(KEY_SIZE, read_cap.size()), read_cap.end()); }
inline Bytes prefix_of(const Bytes& key) { return Bytes(key.begin(), key.begin() + std::min(PREFIX_SIZE, key.size())); }
uint64_t counter(const Bytes& index);
std::array<uint8_t, 32> membership_hash(const std::vector<Bytes>& read_caps);
bool is_sentinel(const Bytes& h);

// ---- GroupChatMessage ----

struct RawField { std::string key; Bytes raw; };

struct Gcm {
    uint64_t version = 0;
    Bytes membership_hash;
    uint64_t msg_type = 0;
    std::optional<std::string> text;
    std::optional<Introduction> introduction;
    std::vector<RawField> other;
    std::optional<Bytes> acks, numbers, introduced_at;

    Bytes to_bytes() const;
    Bytes to_map_bytes() const;
    static std::optional<Gcm> from_bytes(const Bytes& b);
    bool is_control() const { return msg_type == MSG_CONTROL || msg_type == MSG_INTRODUCTION; }
};

// What a member reads of a message: content is the message's own CBOR, kept as written.
struct Message {
    std::optional<Bytes> content;
    std::optional<Introduction> introduction;
    std::vector<Number> numbers;
    std::optional<IntroRef> introduced_at;
    std::vector<Ack> acks;
    std::optional<Bytes> membership_hash;
};

std::optional<Message> message_from_cbor(const Bytes& cbor);
std::optional<Gcm> message_to_gcm(const Message& m, uint64_t version);
std::optional<Bytes> box_of(const Message& m, uint64_t version);
Bytes text_content(const Bytes& membership_hash, const std::string& s, uint64_t version);
std::string text_of(const Bytes& content);

// A box's first byte: F a message, C a chunk, I a long message's chunk stream.
struct Chunk {
    enum Kind { FINAL, CONTINUED, INDIRECTION, UNKNOWN } kind = UNKNOWN;
    Bytes data;  // FINAL, CONTINUED; INDIRECTION: the read cap
    std::optional<uint32_t> total;
};
Bytes frame(const Bytes& cbor);
Chunk unframe(const Bytes& b);

// ---- ReplyWho ----

struct Member { Bytes read_cap, start; };
struct RosterEntry { size_t index; std::optional<size_t> place; };
using Roster = std::vector<RosterEntry>;
struct IntroAt { size_t by; Bytes index; size_t member; };
struct JoinReply {
    std::vector<std::optional<Member>> members;
    std::vector<std::optional<Roster>> rosters;
    std::vector<IntroAt> introductions;
    size_t introducer = 0, newcomer = 0;
};
struct PleaseAdd { std::string name; Bytes read_cap; };

struct ReplyWho {
    std::vector<std::optional<PleaseAdd>> please_adds;
    std::optional<std::vector<std::optional<Roster>>> rosters;
    std::optional<std::vector<IntroAt>> introductions;
    std::optional<size_t> introducer, newcomer;

    std::optional<JoinReply> to_join_reply() const;
    std::optional<JoinReply> without_rosters() const;
    Bytes to_bytes() const;
    Bytes to_map_bytes() const;
    static std::optional<ReplyWho> from_bytes(const Bytes& b);
};

}  // namespace kq1
