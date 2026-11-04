#include "kq1_wire.h"

#include <climits>
#include <cstring>

#include <sodium.h>

namespace kq1 {

// ---- CBOR, byte level ----

void push_header(Bytes& out, uint8_t major, uint64_t n) {
    uint8_t m = (uint8_t)(major << 5);
    if (n < 24) { out.push_back(m | (uint8_t)n); return; }
    int k = n < (1ull << 8) ? 1 : n < (1ull << 16) ? 2 : n < (1ull << 32) ? 4 : 8;
    out.push_back(m | (uint8_t)(k == 1 ? 24 : k == 2 ? 25 : k == 4 ? 26 : 27));
    for (int i = k - 1; i >= 0; i--) out.push_back((uint8_t)(n >> (8 * i)));
}

void push_bytes(Bytes& out, const uint8_t* p, size_t n) {
    push_header(out, 2, n);
    out.insert(out.end(), p, p + n);
}

void push_text(Bytes& out, const std::string& s) {
    push_header(out, 3, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

bool read_header(Span& l, uint8_t& major, uint64_t& n) {
    if (l.empty()) return false;
    uint8_t b = l.p[0], ai = b & 31;
    if (ai >= 28) return false;
    size_t k = ai < 24 ? 0 : (size_t)1 << (ai - 24);
    if (l.n < 1 + k) return false;
    n = ai < 24 ? ai : 0;
    for (size_t i = 0; i < k; i++) n = n << 8 | l.p[1 + i];
    major = b >> 5;
    l.skip(1 + k);
    return true;
}

bool read_uint(Span& l, uint64_t& n) {
    Span s = l;
    uint8_t m;
    if (!read_header(s, m, n) || m != 0) return false;
    l = s;
    return true;
}

static bool read_string(Span& l, uint8_t major, Span& out) {
    Span s = l;
    uint8_t m;
    uint64_t n;
    if (!read_header(s, m, n) || m != major || n > s.n) return false;
    out = {s.p, (size_t)n};
    s.skip((size_t)n);
    l = s;
    return true;
}

bool read_bytes(Span& l, Bytes& out) {
    Span b;
    if (!read_string(l, 2, b)) return false;
    out.assign(b.p, b.p + b.n);
    return true;
}

static bool valid_utf8(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        size_t k = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 9;
        if (k == 9) return false;
        uint32_t cp = k == 0 ? c : k == 1 ? c & 0x1f : k == 2 ? c & 0x0f : c & 0x07;
        for (size_t j = 1; j <= k; j++) {
            if (i + j >= n || (p[i + j] & 0xc0) != 0x80) return false;
            cp = cp << 6 | (p[i + j] & 0x3f);
        }
        static const uint32_t min[] = {0, 0x80, 0x800, 0x10000};
        if (cp < min[k] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += 1 + k;
    }
    return true;
}

bool read_text(Span& l, std::string& out) {
    Span s = l, b;
    if (!read_string(s, 3, b) || !valid_utf8(b.p, b.n)) return false;
    out.assign((const char*)b.p, b.n);
    l = s;
    return true;
}

bool read_array(Span& l, uint64_t& n) {
    Span s = l;
    uint8_t m;
    if (!read_header(s, m, n) || m != 4) return false;
    l = s;
    return true;
}

bool skip_items(size_t fuel, uint64_t count, Span& l) {
    Span s = l;
    for (; count > 0; count--) {
        if (fuel == 0) return false;
        fuel--;
        uint8_t major;
        uint64_t n;
        if (!read_header(s, major, n)) return false;
        switch (major) {
            case 0: case 1: case 7: break;
            case 2: case 3:
                if (n > s.n) return false;
                s.skip((size_t)n);
                break;
            case 4:
                if (!skip_items(fuel, n, s)) return false;
                break;
            case 5:
                if (n > s.n || !skip_items(fuel, 2 * n, s)) return false;
                break;
            default:
                if (!skip_items(fuel, 1, s)) return false;
        }
    }
    l = s;
    return true;
}

bool read_raw(Span& l, Bytes& out) {
    Span s = l;
    if (!skip_items(l.n + 1, 1, s)) return false;
    out.assign(l.p, s.p);
    l = s;
    return true;
}

size_t utf8_chars(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xc0) != 0x80;
    return n;
}

// ---- Term ----

std::optional<size_t> Term::as_nat() const {
    if (kind != INT || negative) return std::nullopt;
    return (size_t)std::min<uint64_t>(value, INT_MAX);
}

const Term* Term::field(const std::string& key) const {
    if (kind != MAP) return nullptr;
    for (size_t i = items.size(); i >= 2; i -= 2)
        if (items[i - 2].kind == TEXT && items[i - 2].str() == key) return &items[i - 1];
    return nullptr;
}

void Term::write(Bytes& out) const {
    switch (kind) {
        case INT: push_header(out, negative ? 1 : 0, value); break;
        case BYTES: push_bytes(out, bytes); break;
        case TEXT: push_header(out, 3, bytes.size()); out.insert(out.end(), bytes.begin(), bytes.end()); break;
        case LIST: push_header(out, 4, items.size()); for (auto& t : items) t.write(out); break;
        case MAP: push_header(out, 5, items.size() / 2); for (auto& t : items) t.write(out); break;
        default: push_null(out);
    }
}

// The Rust reader allows 1000 levels; a device task's stack does not.
static constexpr size_t kMaxDepth = 64;

static uint64_t bignum(const Bytes& b) {
    size_t i = 0;
    while (i < b.size() && b[i] == 0) i++;
    if (b.size() - i > 8) return UINT64_MAX;
    uint64_t n = 0;
    for (; i < b.size(); i++) n = n << 8 | b[i];
    return n;
}

static bool read_term(Span& l, size_t depth, Term& t) {
    if (l.empty()) return false;
    uint8_t first = l.p[0], major = first >> 5, ai = first & 31;
    bool indef = ai == 31;
    if ((indef && (major == 0 || major == 1 || major == 6)) || (ai >= 28 && ai < 31)) return false;
    if (major >= 4 && major <= 6 && depth == 0) return false;
    Span s = l;
    uint64_t n = 0;
    uint8_t m;
    if (indef) s.skip(1);
    else if (!read_header(s, m, n)) return false;
    t = Term{};
    switch (major) {
        case 0: case 1:
            t.kind = Term::INT; t.negative = major == 1; t.value = n;
            break;
        case 2: case 3: {
            Bytes data;
            if (indef) {
                for (;;) {
                    if (s.empty()) return false;
                    if (s.p[0] == 0xff) { s.skip(1); break; }
                    Span chunk;
                    if (!read_string(s, major, chunk)) return false;
                    if (major == 3 && !valid_utf8(chunk.p, chunk.n)) return false;
                    data.insert(data.end(), chunk.p, chunk.p + chunk.n);
                }
            } else {
                if (n > s.n) return false;
                data.assign(s.p, s.p + n);
                s.skip((size_t)n);
            }
            if (major == 3 && !valid_utf8(data.data(), data.size())) return false;
            t.kind = major == 2 ? Term::BYTES : Term::TEXT;
            t.bytes = std::move(data);
            break;
        }
        case 4: case 5: {
            t.kind = major == 4 ? Term::LIST : Term::MAP;
            for (uint64_t i = 0;; i++) {
                if (indef) {
                    if (s.empty()) return false;
                    if (s.p[0] == 0xff) { s.skip(1); break; }
                } else if (i == n) {
                    break;
                }
                for (int k = 0; k < (major == 5 ? 2 : 1); k++) {
                    Term e;
                    if (!read_term(s, depth - 1, e)) return false;
                    t.items.push_back(std::move(e));
                }
            }
            break;
        }
        case 6: {
            Span after = s, str;
            if ((n == 2 || n == 3) && ai < 24 && read_string(after, 2, str)) {
                t.kind = Term::INT;
                t.negative = n == 3;
                t.value = bignum(Bytes(str.p, str.p + str.n));
                s = after;
                break;
            }
            Term inner;
            if (!read_term(s, depth - 1, inner)) return false;
            t.kind = Term::TAG;
            break;
        }
        default:
            if (indef) return false;
            t.kind = ai == 22 ? Term::NUL : Term::OTHER;
    }
    l = s;
    return true;
}

std::optional<Term> Term::read(const uint8_t* p, size_t n) {
    Span l{p, n};
    Term t;
    if (!read_term(l, kMaxDepth, t)) return std::nullopt;
    return t;
}

// ---- fields ----

static size_t width(size_t i) { return i / 8 + 1; }

Bytes acks_to_bytes(const std::vector<Ack>& acks) {
    if (acks.empty()) return {};
    Bytes out{(uint8_t)(acks.size() - 1)};
    size_t w = width(acks.back().member);
    if (acks.size() <= w) {
        for (auto& a : acks) out.push_back((uint8_t)a.member);
    } else {
        Bytes bitmap(w, 0);
        for (auto& a : acks) bitmap[a.member / 8] |= (uint8_t)(1 << (7 - a.member % 8));
        out.insert(out.end(), bitmap.begin(), bitmap.end());
    }
    for (auto& a : acks)
        for (int i = 0; i < 8; i++) out.push_back((uint8_t)(a.idx64 >> (8 * i)));
    return out;
}

std::optional<std::vector<Ack>> acks_from_bytes(const Bytes& field) {
    if (field.empty()) return std::vector<Ack>{};
    size_t count = (size_t)field[0] + 1, rest = field.size() - 1;
    if (rest < count * 8) return std::nullopt;
    size_t w = rest - count * 8;
    const uint8_t* naming = field.data() + 1;
    std::vector<size_t> members;
    if (w == count) {
        for (size_t i = 1; i < w; i++)
            if (naming[i - 1] >= naming[i]) return std::nullopt;
        members.assign(naming, naming + w);
    } else {
        if (count < w || w > 32 || w == 0 || naming[w - 1] == 0) return std::nullopt;
        for (size_t i = 0; i < w * 8; i++)
            if (naming[i / 8] & (1 << (7 - i % 8))) members.push_back(i);
        if (members.size() != count) return std::nullopt;
    }
    std::vector<Ack> out;
    const uint8_t* v = naming + w;
    for (size_t i = 0; i < count; i++) {
        uint64_t x = 0;
        for (int b = 7; b >= 0; b--) x = x << 8 | v[8 * i + b];
        out.push_back({members[i], x});
    }
    return out;
}

Bytes numbers_to_bytes(const std::vector<Number>& numbers) {
    if (numbers.empty()) return {};
    if (numbers.size() == 1 && !numbers[0].prefix) return {(uint8_t)numbers[0].index};
    Bytes out{(uint8_t)numbers[0].index};
    for (auto& n : numbers)
        if (n.prefix) out.insert(out.end(), n.prefix->begin(), n.prefix->end());
    return out;
}

std::vector<Number> numbers_from_bytes(const Bytes& field) {
    if (field.empty()) return {};
    if (field.size() == 1) return {{field[0], std::nullopt}};
    size_t rest = field.size() - 1, first = field[0];
    if (rest % PREFIX_SIZE) return {};
    size_t n = rest / PREFIX_SIZE;
    if (first + n > ROSTER_MAX) return {};
    std::vector<Number> out;
    for (size_t i = 0; i < n; i++) {
        Bytes p(field.begin() + 1 + i * PREFIX_SIZE, field.begin() + 1 + (i + 1) * PREFIX_SIZE);
        for (auto& o : out)
            if (*o.prefix == p) return {};
        out.push_back({first + i, p});
    }
    return out;
}

Bytes introduced_at_to_bytes(const IntroRef& r) {
    Bytes out{(uint8_t)r.introducer};
    out.insert(out.end(), r.index.begin(), r.index.end());
    return out;
}

std::optional<IntroRef> introduced_at_from_bytes(const Bytes& field) {
    if (field.size() != 1 + INDEX_SIZE) return std::nullopt;
    return IntroRef{field[0], Bytes(field.begin() + 1, field.end())};
}

uint64_t counter(const Bytes& index) {
    uint64_t v = 0;
    for (int i = (int)std::min<size_t>(8, index.size()) - 1; i >= 0; i--) v = v << 8 | index[i];
    return v;
}

std::array<uint8_t, 32> membership_hash(const std::vector<Bytes>& read_caps) {
    std::vector<Bytes> keys;
    for (auto& c : read_caps) keys.push_back(key_of(c));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, (const uint8_t*)"KP:membership:v1", 16);
    for (auto& k : keys) crypto_hash_sha256_update(&st, k.data(), k.size());
    std::array<uint8_t, 32> out;
    crypto_hash_sha256_final(&st, out.data());
    return out;
}

bool is_sentinel(const Bytes& h) {
    static const char old[] = "TODOTODOTODOTODOTODOTODOTODOTODO";
    if (h.size() != HASH_SIZE) return false;
    return std::all_of(h.begin(), h.end(), [](uint8_t c) { return c == 0; }) || memcmp(h.data(), old, HASH_SIZE) == 0;
}

// ---- GroupChatMessage ----

static const char* const kOtherKeys[] = {"file_upload", "who", "reply_who", "tally"};

static const char* body_key(uint64_t t) {
    switch (t) {
        case MSG_FILE_UPLOAD: return "file_upload";
        case MSG_WHO: return "who";
        case MSG_REPLY_WHO: return "reply_who";
        case 5: case 6: case 7: case 8: case 9: return "tally";
        default: return nullptr;
    }
}

static bool is_other_key(const std::string& k) {
    for (auto o : kOtherKeys) if (k == o) return true;
    return false;
}

static std::optional<Introduction> intro_of(std::string name, Bytes read_cap) {
    size_t chars = utf8_chars(name);
    if (chars < 1 || chars > MAX_NAME_CHARS || read_cap.size() != READ_CAP_SIZE) return std::nullopt;
    return Introduction{std::move(name), std::move(read_cap)};
}

// File uploads: version 4 writes [payload, filetype, basename], version 3 the map.
static std::optional<Bytes> upload_array(const Bytes& raw) {
    Span l = span(raw);
    uint8_t major;
    uint64_t n;
    if (!read_header(l, major, n) || major != 5) return std::nullopt;
    std::optional<Bytes> p;
    std::optional<std::string> t, b;
    for (uint64_t i = 0; i < n; i++) {
        std::string k;
        if (!read_text(l, k)) return std::nullopt;
        if (k == "payload") {
            Bytes v;
            if (!read_bytes(l, v)) return std::nullopt;
            p = v;
        } else if (k == "filetype" || k == "basename") {
            std::string v;
            if (!read_text(l, v)) return std::nullopt;
            (k == "filetype" ? t : b) = v;
        } else if (!skip_items(l.n + 1, 1, l)) {
            return std::nullopt;
        }
    }
    if (!p || !t || !b) return std::nullopt;
    Bytes out;
    push_header(out, 4, 3);
    push_bytes(out, *p);
    push_text(out, *t);
    push_text(out, *b);
    return out;
}

static bool upload_map(Span& l, Bytes& out) {
    Span s = l;
    uint64_t n;
    Bytes payload;
    std::string filetype, basename;
    if (!read_array(s, n) || n != 3 || !read_bytes(s, payload) || !read_text(s, filetype) || !read_text(s, basename))
        return false;
    out.clear();
    push_header(out, 5, 3);
    push_text(out, "payload");
    push_bytes(out, payload);
    push_text(out, "filetype");
    push_text(out, filetype);
    push_text(out, "basename");
    push_text(out, basename);
    l = s;
    return true;
}

Bytes Gcm::to_bytes() const {
    if (version < CURRENT_VERSION) return to_map_bytes();
    const std::optional<Bytes>* tail[] = {&acks, &numbers, &introduced_at};
    size_t n = 0;
    for (size_t i = 0; i < 3; i++) if (tail[i]->has_value()) n = i + 1;
    Bytes out;
    push_header(out, 4, 4 + n);
    push_uint(out, version);
    push_bytes(out, membership_hash);
    push_uint(out, msg_type);
    const char* key = body_key(msg_type);
    const RawField* raw = nullptr;
    if (key) for (auto& o : other) if (o.key == key) { raw = &o; break; }
    if (msg_type == MSG_TEXT) {
        push_text(out, text.value_or(""));
    } else if (msg_type == MSG_INTRODUCTION) {
        if (introduction) {
            push_header(out, 4, 2);
            push_text(out, introduction->name);
            push_bytes(out, introduction->read_cap);
        } else {
            push_null(out);
        }
    } else if (msg_type == MSG_FILE_UPLOAD && raw) {
        auto a = upload_array(raw->raw);
        const Bytes& b = a ? *a : raw->raw;
        out.insert(out.end(), b.begin(), b.end());
    } else if (raw) {
        out.insert(out.end(), raw->raw.begin(), raw->raw.end());
    } else {
        push_null(out);
    }
    for (size_t i = 0; i < n; i++) {
        if (*tail[i]) push_bytes(out, **tail[i]);
        else push_null(out);
    }
    return out;
}

Bytes Gcm::to_map_bytes() const {
    size_t n = 3 + other.size() + text.has_value() + introduction.has_value() + acks.has_value() +
               numbers.has_value() + introduced_at.has_value();
    Bytes out;
    push_header(out, 5, n);
    push_text(out, "version");
    push_uint(out, version);
    push_text(out, "membership_hash");
    push_bytes(out, membership_hash);
    push_text(out, "msg_type");
    push_uint(out, msg_type);
    if (text) { push_text(out, "text"); push_text(out, *text); }
    if (introduction) {
        push_text(out, "introduction");
        push_header(out, 5, 2);
        push_text(out, "display_name");
        push_text(out, introduction->name);
        push_text(out, "read_cap");
        push_bytes(out, introduction->read_cap);
    }
    for (auto& o : other) {
        push_text(out, o.key);
        out.insert(out.end(), o.raw.begin(), o.raw.end());
    }
    const std::pair<const char*, const std::optional<Bytes>*> tail[] = {
        {"acks", &acks}, {"numbers", &numbers}, {"introduced_at", &introduced_at}};
    for (auto& [k, v] : tail)
        if (*v) { push_text(out, k); push_bytes(out, **v); }
    return out;
}

static std::optional<Gcm> from_array(const Bytes& b) {
    Span l = span(b);
    uint64_t n;
    Gcm g;
    if (!read_array(l, n) || n < 4 || n > 7) return std::nullopt;
    if (!read_uint(l, g.version) || !read_bytes(l, g.membership_hash) || !read_uint(l, g.msg_type)) return std::nullopt;
    if (g.version < CURRENT_VERSION || g.membership_hash.size() != HASH_SIZE || g.msg_type > MSG_CONTROL)
        return std::nullopt;
    if (g.msg_type == MSG_TEXT) {
        std::string s;
        if (!read_text(l, s)) return std::nullopt;
        g.text = s;
    } else if (g.msg_type == MSG_INTRODUCTION) {
        uint64_t k;
        std::string name;
        Bytes cap;
        if (!read_array(l, k) || k != 2 || !read_text(l, name) || !read_bytes(l, cap)) return std::nullopt;
        g.introduction = intro_of(name, cap);
        if (!g.introduction) return std::nullopt;
    } else if (g.msg_type == MSG_FILE_UPLOAD) {
        Bytes raw;
        if (!upload_map(l, raw)) return std::nullopt;
        g.other.push_back({"file_upload", raw});
    } else if (g.msg_type == MSG_CONTROL) {
        if (!is_null(l)) return std::nullopt;
        l.skip(1);
    } else if (is_null(l)) {
        l.skip(1);
    } else {
        Bytes raw;
        if (!read_raw(l, raw)) return std::nullopt;
        g.other.push_back({body_key(g.msg_type), raw});
    }
    std::optional<Bytes>* slots[] = {&g.acks, &g.numbers, &g.introduced_at};
    for (uint64_t i = 0; i < 3 && 4 + i < n; i++) {
        if (is_null(l)) {
            l.skip(1);
        } else {
            Bytes v;
            if (!read_bytes(l, v)) return std::nullopt;
            *slots[i] = v;
        }
    }
    return g;
}

static bool read_intro(Span& l, std::optional<Introduction>& out) {
    Span s = l;
    uint8_t major;
    uint64_t n;
    if (!read_header(s, major, n) || major != 5) return false;
    std::optional<std::string> name;
    std::optional<Bytes> cap;
    for (uint64_t i = 0; i < n; i++) {
        std::string k;
        if (!read_text(s, k)) return false;
        if (k == "display_name") {
            std::string v;
            if (!read_text(s, v)) return false;
            name = v;
        } else if (k == "read_cap") {
            Bytes v;
            if (!read_bytes(s, v)) return false;
            cap = v;
        } else if (!skip_items(s.n + 1, 1, s)) {
            return false;
        }
    }
    if (!name || !cap) return false;
    out = intro_of(*name, *cap);
    if (!out) return false;
    l = s;
    return true;
}

static std::optional<Gcm> from_map(const Bytes& b) {
    Span l = span(b);
    uint8_t major;
    uint64_t n;
    if (!read_header(l, major, n)) return std::nullopt;
    std::optional<uint64_t> version, msg_type;
    std::optional<Bytes> hash;
    Gcm g;
    auto has_other = [&](const char* k) {
        for (auto& o : g.other) if (o.key == k) return true;
        return false;
    };
    for (uint64_t i = 0; i < n; i++) {
        std::string k;
        if (!read_text(l, k)) return std::nullopt;
        if (k == "version") {
            uint64_t v;
            if (!read_uint(l, v)) return std::nullopt;
            version = v;
            continue;
        }
        if (k == "membership_hash") {
            Bytes v;
            if (!read_bytes(l, v)) return std::nullopt;
            hash = v;
            continue;
        }
        bool nullable = k == "msg_type" || k == "text" || k == "introduction" || k == "acks" || k == "numbers" ||
                        k == "introduced_at" || is_other_key(k);
        if (nullable && is_null(l)) { l.skip(1); continue; }
        if (k == "msg_type") {
            uint64_t v;
            if (!read_uint(l, v)) return std::nullopt;
            msg_type = v;
        } else if (k == "text") {
            std::string v;
            if (!read_text(l, v)) return std::nullopt;
            g.text = v;
        } else if (k == "introduction") {
            if (!read_intro(l, g.introduction)) return std::nullopt;
        } else if (k == "acks" || k == "numbers" || k == "introduced_at") {
            Bytes v;
            if (!read_bytes(l, v)) return std::nullopt;
            (k == "acks" ? g.acks : k == "numbers" ? g.numbers : g.introduced_at) = v;
        } else if (is_other_key(k)) {
            Bytes raw;
            if (!read_raw(l, raw)) return std::nullopt;
            g.other.erase(std::remove_if(g.other.begin(), g.other.end(), [&](const RawField& o) { return o.key == k; }),
                          g.other.end());
            g.other.push_back({k, raw});
        } else if (!skip_items(l.n + 1, 1, l)) {
            return std::nullopt;
        }
    }
    if (!msg_type)
        msg_type = g.introduction ? MSG_INTRODUCTION : has_other("file_upload") ? MSG_FILE_UPLOAD
                 : has_other("who") ? MSG_WHO : has_other("reply_who") ? MSG_REPLY_WHO : MSG_TEXT;
    if (!version || !hash || *version > MAP_VERSION || hash->size() != HASH_SIZE || *msg_type > MSG_CONTROL)
        return std::nullopt;
    std::vector<RawField> other;
    for (auto k : kOtherKeys)
        for (auto& o : g.other) if (o.key == k) { other.push_back(o); break; }
    g.version = *version;
    g.membership_hash = *hash;
    g.msg_type = *msg_type;
    g.other = other;
    return g;
}

std::optional<Gcm> Gcm::from_bytes(const Bytes& b) {
    if (b.empty()) return std::nullopt;
    switch (b[0] >> 5) {
        case 4: return from_array(b);
        case 5: return from_map(b);
        default: return std::nullopt;
    }
}

std::optional<Message> message_from_cbor(const Bytes& cbor) {
    auto g = Gcm::from_bytes(cbor);
    if (!g) return std::nullopt;
    Message m;
    if (!g->is_control()) m.content = cbor;
    if (g->msg_type == MSG_INTRODUCTION) m.introduction = g->introduction;
    m.numbers = numbers_from_bytes(g->numbers.value_or(Bytes{}));
    if (g->introduced_at) m.introduced_at = introduced_at_from_bytes(*g->introduced_at);
    m.acks = acks_from_bytes(g->acks.value_or(Bytes{})).value_or(std::vector<Ack>{});
    if (g->is_control() && !is_sentinel(g->membership_hash)) m.membership_hash = g->membership_hash;
    return m;
}

static Bytes stamped(const Message& m) { return m.membership_hash.value_or(Bytes(HASH_SIZE, 0)); }

std::optional<Gcm> message_to_gcm(const Message& m, uint64_t version) {
    Gcm g;
    if (m.content) {
        auto c = Gcm::from_bytes(*m.content);
        if (!c) return std::nullopt;
        g = *c;
    } else {
        g.version = version;
        g.membership_hash = stamped(m);
        g.msg_type = m.introduction ? MSG_INTRODUCTION : MSG_CONTROL;
        g.introduction = m.introduction;
    }
    Bytes a = acks_to_bytes(m.acks), n = numbers_to_bytes(m.numbers);
    g.acks = a.empty() ? std::nullopt : std::optional<Bytes>(a);
    g.numbers = n.empty() ? std::nullopt : std::optional<Bytes>(n);
    g.introduced_at = m.introduced_at ? std::optional<Bytes>(introduced_at_to_bytes(*m.introduced_at)) : std::nullopt;
    return g;
}

std::optional<Bytes> box_of(const Message& m, uint64_t version) {
    auto g = message_to_gcm(m, version);
    if (!g) return std::nullopt;
    return frame(g->to_bytes());
}

Bytes text_content(const Bytes& membership_hash, const std::string& s, uint64_t version) {
    Gcm g;
    g.version = version;
    g.membership_hash = membership_hash;
    g.msg_type = MSG_TEXT;
    g.text = s;
    return g.to_bytes();
}

std::string text_of(const Bytes& content) {
    auto g = Gcm::from_bytes(content);
    if (!g) return "[a message that cannot be read]";
    if (g->msg_type == MSG_TEXT) return g->text.value_or("");
    if (g->msg_type == MSG_FILE_UPLOAD) return "[a file that cannot be read]";
    return "[a message that cannot be shown]";
}

Bytes frame(const Bytes& cbor) {
    Bytes out{'F'};
    out.insert(out.end(), cbor.begin(), cbor.end());
    return out;
}

Chunk unframe(const Bytes& b) {
    Chunk c;
    if (b.empty()) return c;
    Bytes rest(b.begin() + 1, b.end());
    switch (b[0]) {
        case 'F': c.kind = Chunk::FINAL; c.data = rest; break;
        case 'C': c.kind = Chunk::CONTINUED; c.data = rest; break;
        case 'I':
            if (rest.size() == 4 + READ_CAP_SIZE) {
                c.kind = Chunk::INDIRECTION;
                c.total = (uint32_t)rest[0] << 24 | (uint32_t)rest[1] << 16 | (uint32_t)rest[2] << 8 | rest[3];
                c.data.assign(rest.begin() + 4, rest.end());
            } else if (rest.size() == READ_CAP_SIZE) {
                c.kind = Chunk::INDIRECTION;
                c.data = rest;
            }
            break;
    }
    return c;
}

// ---- ReplyWho ----

static const char* const kReplyKeys[] = {"please_adds", "rosters", "introductions", "introducer", "newcomer"};

static bool unique_keys(const std::vector<const PleaseAdd*>& ps) {
    for (size_t i = 0; i < ps.size(); i++)
        for (size_t j = 0; j < i; j++)
            if (key_of(ps[i]->read_cap) == key_of(ps[j]->read_cap)) return false;
    return true;
}

std::optional<JoinReply> ReplyWho::to_join_reply() const {
    size_t places = please_adds.size();
    if (!rosters || !introducer || !newcomer) return std::nullopt;
    if (*newcomer != places || !(*introducer < places && places < ROSTER_MAX)) return std::nullopt;
    if (!please_adds[*introducer] || rosters->size() != places) return std::nullopt;
    std::vector<const PleaseAdd*> ps;
    for (auto& p : please_adds) if (p) ps.push_back(&*p);
    if (!unique_keys(ps)) return std::nullopt;
    for (auto& r : *rosters)
        if (r)
            for (auto& e : *r)
                if (e.index >= ROSTER_MAX || (e.place && *e.place >= places)) return std::nullopt;
    JoinReply j;
    for (auto& p : please_adds)
        j.members.push_back(p ? std::optional<Member>(Member{p->read_cap, start_of(p->read_cap)}) : std::nullopt);
    j.rosters = *rosters;
    if (introductions)
        for (auto& i : *introductions)
            if (i.by < places && i.member < places && i.index.size() == INDEX_SIZE) j.introductions.push_back(i);
    j.introducer = *introducer;
    j.newcomer = *newcomer;
    return j;
}

std::optional<JoinReply> ReplyWho::without_rosters() const {
    if (rosters) return std::nullopt;
    std::vector<const PleaseAdd*> ps;
    for (auto& p : please_adds) if (p) ps.push_back(&*p);
    if (ps.empty() || ps.size() >= ROSTER_MAX || !unique_keys(ps)) return std::nullopt;
    JoinReply j;
    for (auto* p : ps) j.members.push_back(Member{p->read_cap, start_of(p->read_cap)});
    j.rosters.assign(ps.size(), std::nullopt);
    j.introducer = 0;
    j.newcomer = ps.size();
    return j;
}

static std::vector<Term> parts(const ReplyWho& r, bool map) {
    auto please_add = [&](const PleaseAdd& p) {
        if (!map) return Term::list({Term::text(p.name), Term::blob(p.read_cap)});
        Term t;
        t.kind = Term::MAP;
        t.items = {Term::text("display_name"), Term::text(p.name), Term::text("read_cap"), Term::blob(p.read_cap)};
        return t;
    };
    std::vector<Term> pas;
    for (auto& p : r.please_adds) pas.push_back(p ? please_add(*p) : Term{});
    Term rosters;
    if (r.rosters) {
        std::vector<Term> rs;
        for (auto& ro : *r.rosters) {
            if (!ro) { rs.push_back(Term{}); continue; }
            std::vector<Term> es;
            for (auto& e : *ro) es.push_back(Term::list({Term::nat(e.index), e.place ? Term::nat(*e.place) : Term{}}));
            rs.push_back(Term::list(es));
        }
        rosters = Term::list(rs);
    }
    Term intros;
    if (r.introductions) {
        std::vector<Term> is;
        for (auto& i : *r.introductions) is.push_back(Term::list({Term::nat(i.by), Term::blob(i.index), Term::nat(i.member)}));
        intros = Term::list(is);
    }
    return {Term::list(pas), rosters, intros, r.introducer ? Term::nat(*r.introducer) : Term{},
            r.newcomer ? Term::nat(*r.newcomer) : Term{}};
}

Bytes ReplyWho::to_bytes() const {
    auto a = parts(*this, false);
    while (a.size() > 1 && a.back().is_null()) a.pop_back();
    Bytes out;
    Term::list(a).write(out);
    return out;
}

Bytes ReplyWho::to_map_bytes() const {
    auto a = parts(*this, true);
    Term m;
    m.kind = Term::MAP;
    for (size_t i = 0; i < 5; i++)
        if (!a[i].is_null()) { m.items.push_back(Term::text(kReplyKeys[i])); m.items.push_back(a[i]); }
    Bytes out;
    m.write(out);
    return out;
}

static std::optional<PleaseAdd> read_please_add(const Term& t, bool map) {
    const Term *name, *cap;
    if (map) {
        name = t.field("display_name");
        cap = t.field("read_cap");
        if (!name || !cap) return std::nullopt;
    } else {
        if (t.kind != Term::LIST || t.items.size() != 2) return std::nullopt;
        name = &t.items[0];
        cap = &t.items[1];
    }
    if (name->kind != Term::TEXT || cap->kind != Term::BYTES) return std::nullopt;
    size_t chars = utf8_chars(name->str());
    if (chars < 1 || chars > MAX_NAME_CHARS || cap->bytes.size() != READ_CAP_SIZE) return std::nullopt;
    return PleaseAdd{name->str(), cap->bytes};
}

std::optional<ReplyWho> ReplyWho::from_bytes(const Bytes& b) {
    auto t = Term::read(b.data(), b.size());
    if (!t) return std::nullopt;
    bool map;
    const Term* ps[5] = {};
    if (t->kind == Term::LIST && t->items.size() >= 1 && t->items.size() <= 5) {
        map = false;
        for (size_t i = 0; i < t->items.size(); i++) ps[i] = &t->items[i];
    } else if (t->kind == Term::MAP) {
        map = true;
        for (size_t i = 0; i < 5; i++) ps[i] = t->field(kReplyKeys[i]);
    } else {
        return std::nullopt;
    }
    auto part = [&](size_t i) -> const Term* { return ps[i] && !ps[i]->is_null() ? ps[i] : nullptr; };
    ReplyWho r;
    auto pas = part(0);
    if (!pas || pas->kind != Term::LIST) return std::nullopt;
    for (auto& e : pas->items) {
        if (e.is_null()) { r.please_adds.push_back(std::nullopt); continue; }
        auto p = read_please_add(e, map);
        if (!p) return std::nullopt;
        r.please_adds.push_back(p);
    }
    if (auto rs = part(1)) {
        if (rs->kind != Term::LIST) return std::nullopt;
        r.rosters.emplace();
        for (auto& ro : rs->items) {
            if (ro.is_null()) { r.rosters->push_back(std::nullopt); continue; }
            if (ro.kind != Term::LIST) return std::nullopt;
            Roster roster;
            for (auto& e : ro.items) {
                if (e.kind != Term::LIST || e.items.size() != 2) return std::nullopt;
                auto idx = e.items[0].as_nat();
                if (!idx) return std::nullopt;
                std::optional<size_t> place;
                if (!e.items[1].is_null()) {
                    place = e.items[1].as_nat();
                    if (!place) return std::nullopt;
                }
                roster.push_back({*idx, place});
            }
            r.rosters->push_back(roster);
        }
    }
    if (auto is = part(2)) {
        if (is->kind != Term::LIST) return std::nullopt;
        r.introductions.emplace();
        for (auto& e : is->items) {
            if (e.kind != Term::LIST || e.items.size() != 3 || e.items[1].kind != Term::BYTES) return std::nullopt;
            auto by = e.items[0].as_nat(), member = e.items[2].as_nat();
            if (!by || !member) return std::nullopt;
            r.introductions->push_back({*by, e.items[1].bytes, *member});
        }
    }
    for (size_t i : {3, 4}) {
        if (auto v = part(i)) {
            auto n = v->as_nat();
            if (!n) return std::nullopt;
            (i == 3 ? r.introducer : r.newcomer) = *n;
        }
    }
    return r;
}

}  // namespace kq1
