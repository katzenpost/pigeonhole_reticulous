// SPDX-License-Identifier: AGPL-3.0-only
#include "kq1_engine.h"

#include <cstring>

#include <sodium.h>

namespace kq1 {

std::string plain(const Notice& n) {
    auto a = [&](size_t i) { return i < n.args.size() ? n.args[i] : std::string(); };
    switch (n.kind) {
        case NoticeKind::NameTaken: return "you already have a group called " + a(0);
        case NoticeKind::NameTakenJoining:
            return "you already have a group called " + a(0) + ": to join someone else's group, give it a name of its own";
        case NoticeKind::Minting: return "minting a voucher";
        case NoticeKind::NewGroup: return "a new group: adding a member's voucher code adds them";
        case NoticeKind::VoucherMinted: return "give the voucher code to a member of the group (it is also in " + a(2) + ")";
        case NoticeKind::ReplyUnreadable: return "the reply to our voucher cannot be read";
        case NoticeKind::Joined: return "joined the group";
        case NoticeKind::NotJoinedYet: return "join or create the group first";
        case NoticeKind::AlreadyAdding: return "already adding a member";
        case NoticeKind::NotAVoucher: return "that is not a voucher code";
        case NoticeKind::ReadingVoucher: return "reading the voucher";
        case NoticeKind::Added: return "added ~" + a(0) + ": they now read everything said in this group";
        case NoticeKind::NotJoined: return "this conversation has not been joined yet";
        case NoticeKind::FileTooLarge: return "that file is too large to send: at most " + a(0);
        case NoticeKind::SendLost: return "a message could not be sent: what it held was lost";
        case NoticeKind::FileTooLargeToFetch: return a(0) + " sent a file too large to fetch";
        case NoticeKind::Scanning: return "scanning for missed messages";
        case NoticeKind::NoLongerMember: return "that member is no longer in this group";
        case NoticeKind::StoppedReading:
            return "stopped reading " + a(0) + " and deleted what they sent; they can still read this group";
        case NoticeKind::Renamed: return "renamed from " + a(0);
        case NoticeKind::ActionFailed: return a(0);
        case NoticeKind::TooLong: return "that message is too long to send";
        case NoticeKind::NotWritten: return "a message could not be written";
        case NoticeKind::MemberAdded: return a(0) + " added ~" + a(1);
        case NoticeKind::Unreadable: return a(0) + " sent a message that cannot be read";
        case NoticeKind::NotFetched: return a(0) + " sent a message that could not be fetched";
        case NoticeKind::Desync:
            return "a box of our stream holds something else: is this group used from another client?";
    }
    return "";
}

std::string size_text(size_t n) {
    constexpr size_t MB = 1024 * 1024;
    if (n < 1024) return std::to_string(n) + " bytes";
    if (n < MB) return std::to_string((n + 512) / 1024) + " KB";
    return std::to_string(n / MB) + "." + std::to_string(n % MB * 10 / MB) + " MB";
}

uint64_t backoff(size_t n) { return std::min<uint64_t>(60000, 2000ull << std::min<size_t>(n, 5)); }

std::string voucher_code(const Bytes& v) {
    std::string s(sodium_base64_encoded_len(v.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(s.data(), s.size(), v.data(), v.size(), sodium_base64_VARIANT_ORIGINAL);
    s.resize(strlen(s.c_str()));
    return s;
}

// Whitespace is skipped: hushphone shows a code in groups of eight.
static std::optional<Bytes> from_code(const std::string& code) {
    Bytes b(code.size());
    size_t n = 0;
    const char* end = nullptr;
    if (sodium_base642bin(b.data(), b.size(), code.data(), code.size(), " \t\r\n", &n, &end,
                          sodium_base64_VARIANT_ORIGINAL) != 0 ||
        end != code.data() + code.size() || n != 32)
        return std::nullopt;
    b.resize(n);
    return b;
}

std::string sanitize(const std::string& name) {
    std::string out;
    for (size_t i = 0; i < name.size();) {
        unsigned char c = name[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
        for (size_t k = 1; k < len && i + k < name.size(); k++) cp = cp << 6 | (name[i + k] & 0x3f);
        if (!(cp < 0x20 || (cp >= 0x7f && cp <= 0x9f))) out += name.substr(i, len);
        i += len;
    }
    return out.empty() ? "unnamed" : out;
}

// Each char, not each byte: a multibyte char becomes one '_'.
static std::string slug(const std::string& group) {
    std::string out;
    for (size_t i = 0; i < group.size();) {
        unsigned char c = group[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        out += len == 1 && isalnum(c) ? (char)c : '_';
        i += len;
    }
    return out;
}

static std::string voucher_file(const std::string& group) { return "voucher-" + slug(group) + ".txt"; }

static std::string file_text(const Attachment& a) { return "[file " + a.name + ", " + size_text(a.size) + "]"; }

static std::string attachment_file(const Chat& c, size_t id, int64_t now, const std::string& name) {
    return "attachments/" + slug(c.name) + "/" + std::to_string(now) + "-" + std::to_string(id) + "-" + name;
}

static uint8_t voucher_form(uint64_t version) { return version == MAP_VERSION ? 1 : 2; }

static std::optional<Message> message_of(const Bytes& plaintext) {
    auto c = unframe(plaintext);
    if (c.kind != Chunk::FINAL) return std::nullopt;
    return message_from_cbor(c.data);
}

static std::pair<ReadResult, std::optional<Index>> read_result(const Outcome& o) {
    switch (o.kind) {
        case Outcome::NOT_FOUND: return {ReadResult{ReadResult::NOT_FOUND, std::nullopt}, std::nullopt};
        case Outcome::TOMBSTONE: return {ReadResult{ReadResult::TOMBSTONE, std::nullopt}, o.next};
        case Outcome::DATA: return {ReadResult{ReadResult::DATA, message_of(o.plaintext)}, o.next};
    }
    return {};
}

static ReplyWho reply_who_of(const JoinReply& r, const std::function<std::optional<std::string>(const Bytes&)>& name) {
    ReplyWho w;
    for (auto& m : r.members) {
        if (!m) { w.please_adds.push_back(std::nullopt); continue; }
        Bytes key = key_of(m->read_cap);
        auto n = name(key);
        if (!n) { w.please_adds.push_back(std::nullopt); continue; }
        Bytes cap = key;
        cap.insert(cap.end(), m->start.begin(), m->start.end());
        w.please_adds.push_back(PleaseAdd{*n, cap});
    }
    w.rosters = r.rosters;
    w.introductions = r.introductions;
    w.introducer = r.introducer;
    w.newcomer = r.newcomer;
    return w;
}

// ---- state ----

bool Chat::acknowledged(const Line& l) const {
    if (!group || !l.idx64) return false;
    for (auto& [_, s] : group->ledger.acked_by)
        if (*l.idx64 <= s) return true;
    return false;
}

Job* Chat::job(size_t id) {
    for (auto& j : jobs) if (j.id == id) return &j;
    return nullptr;
}

std::vector<std::string> Chat::files() const {
    std::vector<std::string> fs;
    for (auto& l : log) if (l.attachment) fs.push_back(l.attachment->file);
    for (auto& j : jobs) if (j.upload) fs.push_back(j.upload->file);
    return fs;
}

Chat* State::chat(const ChatId& id) {
    for (auto& c : chats) if (c.id == id) return &c;
    return nullptr;
}
const Chat* State::chat(const ChatId& id) const {
    for (auto& c : chats) if (c.id == id) return &c;
    return nullptr;
}

bool State::wanted(const Action& a) const {
    const Chat* c = chat(a.chat);
    switch (a.kind) {
        case Action::AWAIT_REPLY: return c && c->joining && c->joining->secret_key == a.secret_key;
        case Action::READ_VOUCHER: return c && c->inducting && !c->inducting->payload && c->inducting->voucher == a.voucher;
        case Action::WRITE: case Action::READ: case Action::SCAN_READ: case Action::WALK: case Action::DERIVE:
        case Action::SUBSTREAM: case Action::WRITE_CHUNK: case Action::CHUNK_INDEXES: case Action::READ_CHUNK:
            return c != nullptr;
        default: return true;
    }
}

void Engine::Flight::forget(const ChatId& chat) {
    writing.erase(chat);
    for (auto it = reading.begin(); it != reading.end();) it = it->first == chat ? reading.erase(it) : std::next(it);
    auto drop = [&](auto& m) {
        for (auto it = m.begin(); it != m.end();) it = it->first == chat ? m.erase(it) : std::next(it);
    };
    auto drop_keyed = [&](auto& m) {
        for (auto it = m.begin(); it != m.end();) it = it->first.first == chat ? m.erase(it) : std::next(it);
    };
    drop_keyed(misses);
    drop_keyed(fetched);
    drop(uploading);
    drop(loading);
    drop_keyed(ahead);
    drop_keyed(chunks_out);
    drop_keyed(landed);
    drop_keyed(blobs);
}

// ---- one step's context, as engine/core.rs Cx ----

struct Engine::Cx {
    StepResult& out;
    Flight& flight;
    int64_t now;
    size_t max;
    const std::string& me;
    uint64_t version;
    size_t max_file;

    void emit(Action a) { out.actions.push_back(std::move(a)); }
    void keep(std::string file, Bytes bytes) { out.files.push_back({std::move(file), std::move(bytes)}); }
    void dirty(const Chat& c) { out.changes.chats.insert(c.id); }
    void line_changed(const Chat& c, size_t id) { out.changes.lines[c.id].insert(id); }
    void line_dropped(const Chat& c, size_t id) {
        auto it = out.changes.lines.find(c.id);
        if (it != out.changes.lines.end()) it->second.erase(id);
        out.changes.dropped[c.id].insert(id);
    }
    std::string label(const Chat& c, const Key& key) {
        if (c.group && c.group->me() == key) return "me";
        auto n = c.names.get(key);
        return "~" + (n ? *n : std::string("?"));
    }
    size_t say(Chat& c, Line l) {
        l.id = c.fresh();
        l.time = now;
        size_t id = l.id;
        c.log.push_back(std::move(l));
        dirty(c);
        line_changed(c, id);
        if (c.log.size() > MAX_LOG) {
            size_t n = c.log.size() - MAX_LOG;
            for (size_t k = 0; k < n; k++) {
                if (c.log[k].attachment) del(c.log[k].attachment->file);
                line_dropped(c, c.log[k].id);
            }
            c.log.erase(c.log.begin(), c.log.begin() + n);
        }
        return id;
    }
    void del(const std::string& file) {
        Action a(Action::DELETE);
        a.file = file;
        emit(a);
    }
    void notice(Chat& c, NoticeKind kind, std::vector<std::string> args) {
        Line l;
        l.notice = Notice{kind, std::move(args)};
        say(c, std::move(l));
    }
    // Queue outgoing, each written after the one before; line shows the one carrying content,
    // into which the bytes of a file it carries, payload, are put. A message too long for a box
    // goes to a substream, written before the I box naming it.
    void enqueue(Chat& c, const std::vector<Outgoing>& outgoing, const std::optional<Bytes>& content,
                 std::optional<size_t> line, const Bytes* payload = nullptr) {
        for (auto& o : outgoing) {
            if (!c.group) continue;
            bool mine = content && o.message.content == content;
            auto b = box_of(o.message, version);
            if (b && payload && mine) {
                auto full = put_payload(Bytes(b->begin() + 1, b->end()), *payload);
                b = full ? std::optional<Bytes>(frame(*full)) : std::nullopt;
            }
            if (b) {
                Job j;
                j.token = o.token;
                if (mine) j.line = line;
                if (b->size() <= max) {
                    j.id = c.fresh();
                    j.box = std::move(*b);
                    c.jobs.push_back(std::move(j));
                    dirty(c);
                    continue;
                }
                Bytes blob(b->begin() + 1, b->end());
                if (blob.size() <= max_file + max) {
                    j.id = c.fresh();
                    Upload u;
                    u.file = "spool/" + slug(c.name) + "-" + std::to_string(now) + "-" + std::to_string(j.id) + ".cbor";
                    u.box_size = max;
                    u.total = chunk_count(blob.size(), max);
                    keep(u.file, blob);
                    flight.blobs[{c.id, j.id}] = std::move(blob);
                    j.upload = std::move(u);
                    c.jobs.push_back(std::move(j));
                    dirty(c);
                    continue;
                }
            }
            c.group->abandoned(o.token);
            notice(c, mine ? NoticeKind::TooLong : NoticeKind::NotWritten, {});
        }
    }
    void rewrite(Chat& c, Index index, Bytes b) {
        Job j;
        j.id = c.fresh();
        j.index = std::move(index);
        j.box = std::move(b);
        c.jobs.push_back(j);
        dirty(c);
    }
    // A line for content from sender, and the file it carries kept.
    void deliver(Chat& c, const Key& sender, const Index& index, const Bytes& content) {
        Line l;
        l.sender = sender;
        l.index = index;
        auto g = Gcm::from_bytes(content);
        auto f = g ? file_of(*g) : std::nullopt;
        if (!f) {
            l.text = text_of(content);
            say(c, l);
            return;
        }
        auto it = flight.arriving.find({sender, index});
        if (it != flight.arriving.end()) f->payload = it->second;
        std::string name = safe_name(f->basename);
        Attachment a{name, f->filetype, f->payload.size(), attachment_file(c, c.next_id, now, name)};
        if (f->payload.size() > max_file) {
            l.text = "[file " + name + ", " + size_text(a.size) + ", too large to keep]";
            say(c, l);
            return;
        }
        keep(a.file, std::move(f->payload));
        l.text = file_text(a);
        l.attachment = std::move(a);
        say(c, l);
    }
    template <class Keep>
    void drop_lines(Chat& c, Keep keep) {
        size_t before = c.log.size();
        std::vector<Line> kept;
        for (auto& l : c.log) {
            if (keep(l)) { kept.push_back(l); continue; }
            if (l.attachment) del(l.attachment->file);
            line_dropped(c, l.id);
        }
        c.log = kept;
        if (c.log.size() != before) dirty(c);
    }
    void effects(Chat& c, const std::vector<Effect>& es) {
        for (auto& e : es) {
            switch (e.kind) {
                case Effect::WRITE_BOX: {
                    auto b = e.payload.is_bytes ? std::optional<Bytes>(e.payload.bytes) : box_of(e.payload.message, version);
                    if (b) rewrite(c, e.index, *b);
                    break;
                }
                case Effect::WRITE_TOMBSTONE: rewrite(c, e.index, {}); break;
                case Effect::READ_BOX: {
                    Action a(Action::SCAN_READ);
                    a.chat = c.id;
                    a.stream = e.stream;
                    a.read_cap = e.read_cap;
                    a.index = e.index;
                    emit(a);
                    break;
                }
                case Effect::DERIVE_INDEXES: {
                    Action a(Action::DERIVE);
                    a.chat = c.id;
                    a.stream = e.stream;
                    a.start = e.start;
                    a.count = e.count;
                    emit(a);
                    break;
                }
                case Effect::WALK: {
                    Action a(Action::WALK);
                    a.chat = c.id;
                    a.stream = e.stream;
                    a.start = e.start;
                    a.targets = e.targets;
                    a.keep = e.keep;
                    emit(a);
                    break;
                }
                case Effect::DELIVER: deliver(c, e.sender, e.index, e.content); break;
                case Effect::EXPIRE:
                    drop_lines(c, [&](const Line& l) { return !(l.sender == e.sender && l.index == e.index); });
                    break;
                case Effect::SEND_JOIN_REPLY: break;
            }
        }
    }
    void remark(Chat& c, const Key& stream, const Bytes& plaintext) {
        auto ch = unframe(plaintext);
        if (ch.kind == Chunk::FINAL) {
            auto m = message_from_cbor(ch.data);
            if (!m || !m->introduction) return;
            auto& i = *m->introduction;
            Key k = key_of(i.read_cap);
            if (!c.names.contains(k)) {
                c.names.insert(k, i.name);
                dirty(c);
            }
            if (c.group && c.group->me() == k) return;
            notice(c, NoticeKind::MemberAdded, {label(c, stream), i.name});
        } else if (ch.kind == Chunk::CONTINUED) {
            notice(c, NoticeKind::Unreadable, {label(c, stream)});
        }
    }
    void start_reads(Chat& c) {
        if (!c.group) return;
        std::vector<std::tuple<Key, Bytes, Index>> rs;
        for (auto& [_, r] : c.group->readers) rs.push_back({r.stream, r.read_cap, r.next});
        for (auto& [stream, read_cap, next] : rs) {
            auto k = std::make_pair(c.id, stream);
            if (flight.reading.count(k)) continue;
            bool fetching = false;
            for (auto& d : c.downloads) fetching |= d.stream == stream;
            if (fetching) continue;
            Index at;
            if (auto cur = c.cursors.get(stream)) {
                at = *cur;
            } else {
                c.cursors.insert(stream, next);
                dirty(c);
                at = next;
            }
            flight.reading.insert(k);
            Action a(Action::READ);
            a.chat = c.id;
            a.stream = stream;
            a.read_cap = read_cap;
            a.index = at;
            emit(a);
        }
    }
    void start_write(Chat& c) {
        if (flight.writing.count(c.id) || c.write_cap.empty() || c.jobs.empty()) return;
        if (c.inducting && c.inducting->sealing) return;
        if (c.jobs[0].upload && c.jobs[0].upload->sent < c.jobs[0].upload->total) return;
        if (!c.jobs[0].index) {
            c.jobs[0].index = c.next_write;
            dirty(c);
        }
        flight.writing.insert(c.id);
        Action a(Action::WRITE);
        a.chat = c.id;
        a.job = c.jobs[0].id;
        a.write_cap = c.write_cap;
        a.index = *c.jobs[0].index;
        a.box = c.jobs[0].box;
        emit(a);
    }
    void start_induction(Chat& c) {
        if (!c.inducting || !c.group) return;
        auto& i = *c.inducting;
        if (!i.payload || i.sealing || i.sealed) return;
        for (auto& j : c.jobs) if (j.token) return;
        Key m = c.group->me();
        auto who = reply_who_of(c.group->reply(c.next_write), [&](const Bytes& key) -> std::optional<std::string> {
            if (key == m) return me;
            auto n = c.names.get(key);
            return n ? std::optional<std::string>(*n) : std::nullopt;
        });
        Action a(Action::SEAL);
        a.chat = c.id;
        a.voucher = i.voucher;
        a.payload = *i.payload;
        a.who_reply = voucher_version(*i.payload) == 1 ? who.to_map_bytes() : who.to_bytes();
        emit(a);
        i.sealing = true;
        dirty(c);
    }
    // Write the next boxes of each upload, making its substream, reading its message back and
    // deriving their indexes first.
    void start_uploads(Chat& c) {
        for (auto& j : c.jobs) {
            if (!j.upload) continue;
            Upload& u = *j.upload;
            JobKey key{c.id, j.id};
            if (u.sent >= u.total || flight.uploading.count(key)) continue;
            Action a(Action::SUBSTREAM);
            a.chat = c.id;
            a.job = j.id;
            if (u.write_cap.empty()) {
                flight.uploading.insert(key);
                emit(a);
                continue;
            }
            auto blob = flight.blobs.find(key);
            if (blob == flight.blobs.end()) {
                a.kind = Action::LOAD;
                a.file = u.file;
                if (flight.loading.insert(key).second) emit(a);
                continue;
            }
            size_t want = std::min(WINDOW, u.total - u.sent);
            auto& ahead = flight.ahead[key];
            if (ahead.size() < want) {
                flight.uploading.insert(key);
                a.kind = Action::CHUNK_INDEXES;
                a.start = u.next;
                a.count = std::min(2 * WINDOW, u.total - u.sent) - 1;
                emit(a);
                continue;
            }
            auto& out = flight.chunks_out[key];
            auto& landed = flight.landed[key];
            for (size_t i = 0; i < want; i++) {
                size_t n = u.sent + i;
                bool there = out.count(n);
                for (auto& l : landed) there |= l.first == n;
                if (there) continue;
                out.insert(n);
                Action w(Action::WRITE_CHUNK);
                w.chat = c.id;
                w.job = j.id;
                w.n = n;
                w.write_cap = u.write_cap;
                w.index = ahead[i];
                w.box = substream_box(blob->second, u.box_size, n);
                emit(std::move(w));
            }
        }
    }
    // Read the next box of each download.
    void start_downloads(Chat& c) {
        for (auto& d : c.downloads) {
            StreamKey k{c.id, d.stream};
            if (flight.reading.count(k)) continue;
            auto f = flight.fetched.find(k);
            flight.reading.insert(k);
            Action a(Action::READ_CHUNK);
            a.chat = c.id;
            a.stream = d.stream;
            a.read_cap = d.read_cap;
            a.index = f == flight.fetched.end() ? start_of(d.read_cap) : f->second.first;
            emit(std::move(a));
        }
    }
    void stop_reading(const Chat& c, const Key& stream) { flight.reading.erase({c.id, stream}); }
    size_t missed(const Chat& c, const Key& stream) { return flight.misses[{c.id, stream}]++; }
    void hit(const Chat& c, const Key& stream) { flight.misses.erase({c.id, stream}); }
    void follow_reader(Chat& c, const Key& stream) {
        if (!c.group) return;
        auto r = c.group->readers.get(stream);
        auto at = c.cursors.get(stream);
        if (!r || !at) return;
        if (counter(*at) < counter(r->next)) {
            c.cursors.insert(stream, r->next);
            dirty(c);
        }
    }
    // A download is over: tell the member what came of it, in its place in the stream.
    void finish(Chat& c, Download d, std::optional<Bytes> blob) {
        flight.fetched.erase({c.id, d.stream});
        c.downloads.erase(std::remove_if(c.downloads.begin(), c.downloads.end(),
                                         [&](const Download& x) { return x.stream == d.stream; }),
                          c.downloads.end());
        dirty(c);
        std::optional<Bytes> without;
        if (blob) {
            if (auto t = take_payload(*blob)) {
                flight.arriving.clear();
                flight.arriving[{d.stream, d.index}] = std::move(t->second);
                without = std::move(t->first);
            } else {
                without = std::move(blob);
            }
        }
        auto m = without ? message_from_cbor(*without) : std::nullopt;
        if (m) remark(c, d.stream, frame(*without));
        else notice(c, NoticeKind::NotFetched, {label(c, d.stream)});
        if (c.group) effects(c, c.group->observe(d.stream, d.index, m, d.next));
        flight.arriving.clear();
    }
    void wrote_chunk(Chat& c, size_t job, size_t n, const Index& next) {
        JobKey key{c.id, job};
        auto out = flight.chunks_out.find(key);
        if (out != flight.chunks_out.end()) out->second.erase(n);
        Job* j = c.job(job);
        if (!j || !j->upload) return;
        Upload& u = *j->upload;
        if (n < u.sent) return;
        auto& landed = flight.landed[key];
        landed.push_back({n, next});
        // Count the boxes written in a row from the first not yet counted.
        for (;;) {
            auto it = std::find_if(landed.begin(), landed.end(), [&](auto& l) { return l.first == u.sent; });
            if (it == landed.end()) break;
            Index after = std::move(it->second);
            landed.erase(it);
            auto a = flight.ahead.find(key);
            if (a != flight.ahead.end() && !a->second.empty()) a->second.erase(a->second.begin());
            u.sent++;
            u.next = std::move(after);
        }
        bool done = u.sent >= u.total;
        if (done) j->box = indirection(u.total, Bytes(u.write_cap.begin() + KEY_SIZE, u.write_cap.end()));
        dirty(c);
        if (done) {
            flight.blobs.erase(key);
            flight.ahead.erase(key);
            flight.landed.erase(key);
            del(u.file);
        }
    }
    void read_chunk(Chat& c, const Key& stream, const Index& index, const Outcome& outcome) {
        stop_reading(c, stream);
        auto di = std::find_if(c.downloads.begin(), c.downloads.end(), [&](const Download& d) { return d.stream == stream; });
        if (di == c.downloads.end()) return;
        Download d = *di;
        StreamKey k{c.id, stream};
        auto f = flight.fetched.find(k);
        if ((f == flight.fetched.end() ? start_of(d.read_cap) : f->second.first) != index) return;
        if (outcome.kind == Outcome::NOT_FOUND) { wake(c, stream); return; }
        if (outcome.kind == Outcome::TOMBSTONE) { finish(c, d, std::nullopt); return; }
        hit(c, stream);
        std::vector<Bytes> parts;
        if (f != flight.fetched.end()) parts = std::move(f->second.second);
        uint8_t tag = outcome.plaintext.empty() ? 0 : outcome.plaintext[0];
        parts.push_back(outcome.plaintext);
        if (tag == 'F') {
            finish(c, d, assemble(parts));
        } else if (tag == 'C' && parts.size() < max_chunks(max, max_file) && (!d.total || parts.size() < *d.total)) {
            flight.fetched[k] = {outcome.next, std::move(parts)};
        } else {
            finish(c, d, std::nullopt);
        }
    }
    void cancel_held(Chat& c) {
        if (c.group && c.group->ledger.induction == Induction::INDUCTING) c.group->cancel_induction();
    }
    void wake(Chat& c, const Key& stream) {
        flight.reading.insert({c.id, stream});
        Action a(Action::WAKE);
        a.chat = c.id;
        a.stream = stream;
        a.ms = backoff(missed(c, stream));
        emit(a);
    }
    void read(Chat& c, const Key& stream, const Index& index, const Outcome& outcome) {
        stop_reading(c, stream);
        auto cur = c.cursors.get(stream);
        if (!cur || *cur != index) return;
        if (outcome.kind == Outcome::NOT_FOUND) {
            wake(c, stream);
            return;
        }
        hit(c, stream);
        if (outcome.kind == Outcome::TOMBSTONE) {
            c.cursors.insert(stream, outcome.next);
            dirty(c);
            return;
        }
        dirty(c);
        auto ch = unframe(outcome.plaintext);
        if (ch.kind == Chunk::INDIRECTION) {
            if (ch.total.value_or(0) <= max_chunks(max, max_file)) {
                c.downloads.push_back({stream, index, outcome.next, ch.data,
                                       ch.total ? std::optional<size_t>(*ch.total) : std::nullopt});
                c.cursors.insert(stream, outcome.next);
                return;
            }
            notice(c, NoticeKind::FileTooLargeToFetch, {label(c, stream)});
            if (c.group) effects(c, c.group->observe(stream, index, std::nullopt, outcome.next));
            c.cursors.insert(stream, outcome.next);
            return;
        }
        remark(c, stream, outcome.plaintext);
        if (c.group) effects(c, c.group->observe(stream, index, message_of(outcome.plaintext), outcome.next));
        c.cursors.insert(stream, outcome.next);
    }
};

static std::optional<std::pair<Group, ReplyWho>> joined_from(const Opened& o) {
    auto who = ReplyWho::from_bytes(o.who_reply);
    if (!who || o.mutated_write_cap.size() < KEY_SIZE) return std::nullopt;
    auto reply = who->to_join_reply();
    if (!reply) reply = who->without_rosters();
    if (!reply) return std::nullopt;
    Bytes own(o.mutated_write_cap.begin() + KEY_SIZE, o.mutated_write_cap.end());
    for (auto& m : reply->members)
        if (m && key_of(m->read_cap) == key_of(own)) return std::nullopt;
    return std::make_pair(Group::joined(own, *reply, Retention{}, 256), *who);
}

void Engine::pump(StepResult& out, int64_t now) {
    Cx cx{out, flight_, now, max_plaintext, state.me, wire_version, max_file};
    for (auto& c : state.chats) {
        cx.start_write(c);
        cx.start_reads(c);
        cx.start_induction(c);
        cx.start_uploads(c);
        cx.start_downloads(c);
    }
}

StepResult Engine::step(int64_t now, const Input& input) {
    StepResult out;
    handle(out, now, input);
    pump(out, now);
    return out;
}

StepResult Engine::resume(int64_t now) {
    StepResult out;
    for (auto& c : state.chats) {
        if (c.write_cap.empty()) {
            Action a(Action::CREATE);
            a.chat = c.id;
            a.joining = c.joining.has_value();
            out.actions.push_back(a);
            continue;
        }
        if (c.joining) {
            if (c.joining->code.empty()) {
                Action a(Action::MINT);
                a.chat = c.id;
                a.write_cap = c.write_cap;
                a.name = state.me;
                a.form = voucher_form(wire_version);
                out.actions.push_back(a);
            } else {
                Action a(Action::AWAIT_REPLY);
                a.chat = c.id;
                a.secret_key = c.joining->secret_key;
                a.voucher_read_cap = c.joining->voucher_read_cap;
                a.box1 = c.joining->box1;
                a.write_cap = c.write_cap;
                a.version = c.joining->version;
                out.actions.push_back(a);
            }
        }
        if (c.inducting) {
            auto& i = *c.inducting;
            if (!i.payload) {
                Action a(Action::READ_VOUCHER);
                a.chat = c.id;
                a.voucher = i.voucher;
                out.actions.push_back(a);
            } else if (i.sealed) {
                Action a(Action::PUBLISH);
                a.chat = c.id;
                a.voucher_write_cap = i.voucher_write_cap;
                a.box1 = i.box1;
                a.reply = i.sealed->reply;
                a.version = i.sealed->version;
                out.actions.push_back(a);
            } else if (i.sealing) {
                i.sealing = false;
                out.changes.chats.insert(c.id);
            }
        }
    }
    pump(out, now);
    return out;
}

void Engine::handle(StepResult& out, int64_t now, const Input& input) {
    const std::string me = state.me;
    Cx cx{out, flight_, now, max_plaintext, me, wire_version, max_file};
    switch (input.kind) {
        case Input::CREATE:
        case Input::JOIN: {
            bool joining = input.kind == Input::JOIN;
            for (auto& c : state.chats)
                if (c.name == input.name) {
                    cx.notice(c, joining ? NoticeKind::NameTakenJoining : NoticeKind::NameTaken, {input.name});
                    return;
                }
            Chat c;
            c.id = input.chat;
            c.name = input.name;
            if (joining) c.joining = Joining{};
            cx.dirty(c);
            Action a(Action::CREATE);
            a.chat = input.chat;
            a.joining = joining;
            cx.emit(a);
            state.chats.push_back(std::move(c));
            return;
        }
        case Input::LEAVE: {
            auto it = std::find_if(state.chats.begin(), state.chats.end(), [&](const Chat& c) { return c.id == input.chat; });
            if (it == state.chats.end()) return;
            for (auto& f : it->files()) cx.del(f);
            state.chats.erase(it);
            flight_.forget(input.chat);
            out.changes.chats.erase(input.chat);
            out.changes.lines.erase(input.chat);
            out.changes.dropped.erase(input.chat);
            out.changes.removed.insert(input.chat);
            return;
        }
        case Input::RENAME: {
            bool taken = false;
            for (auto& c : state.chats) taken |= c.name == input.to && c.id != input.chat;
            Chat* c = state.chat(input.chat);
            if (!c || input.to.empty() || input.to == c->name) return;
            cx.dirty(*c);
            if (taken) {
                cx.notice(*c, NoticeKind::NameTaken, {input.to});
                return;
            }
            std::string old = c->name;
            c->name = input.to;
            cx.notice(*c, NoticeKind::Renamed, {old});
            return;
        }
        case Input::EPOCH: {
            epoch = std::max(epoch, input.n);
            for (auto& c : state.chats) {
                if (!c.group || c.group->epoch >= input.n) continue;
                Event e(Event::EPOCH_CHANGED);
                e.epoch = input.n;
                auto es = c.group->handle(e);
                cx.dirty(c);
                cx.effects(c, es);
            }
            return;
        }
        default: break;
    }
    const ChatId& id = input.chat;
    if (input.kind == Input::WROTE) flight_.writing.erase(id);
    if (input.kind == Input::LOADED) flight_.loading.erase({id, input.job});
    if (input.kind == Input::SUBSTREAM || input.kind == Input::CHUNK_INDEXES) flight_.uploading.erase({id, input.job});
    if (input.kind == Input::WAKE) {
        flight_.reading.erase({id, input.stream});
        return;
    }
    Chat* cp = state.chat(id);
    if (!cp) return;
    Chat& c = *cp;
    if (input.kind != Input::READ && input.kind != Input::READ_CHUNK && input.kind != Input::CHUNK_INDEXES &&
        input.kind != Input::WROTE_CHUNK)
        cx.dirty(c);
    switch (input.kind) {
        case Input::CREATED: {
            c.write_cap = input.keypair.write_cap;
            c.next_write = input.keypair.first;
            if (input.joining) {
                Action a(Action::MINT);
                a.chat = id;
                a.write_cap = c.write_cap;
                a.name = me;
                a.form = voucher_form(wire_version);
                cx.emit(a);
                cx.notice(c, NoticeKind::Minting, {});
                return;
            }
            Group g = Group::found(input.keypair.read_cap, {input.keypair.read_cap}, Retention{}, 256);
            Event e(Event::EPOCH_CHANGED);
            e.epoch = epoch;
            auto es = g.handle(e);
            c.group = std::move(g);
            cx.effects(c, es);
            cx.notice(c, NoticeKind::NewGroup, {});
            return;
        }
        case Input::MINTED: {
            std::string code = voucher_code(input.minted.voucher);
            Action a(Action::AWAIT_REPLY);
            a.chat = id;
            a.secret_key = input.minted.secret_key;
            a.voucher_read_cap = input.minted.read_cap;
            a.box1 = input.box1;
            a.write_cap = c.write_cap;
            a.version = input.minted.version;
            cx.emit(a);
            c.joining = Joining{code, input.minted.secret_key, input.minted.read_cap, input.box1, input.minted.version};
            std::string file = voucher_file(c.name);
            cx.keep(file, Bytes(code.begin(), code.end()));
            cx.out.files.back().bytes.push_back('\n');
            cx.notice(c, NoticeKind::VoucherMinted, {code, c.name, file});
            return;
        }
        case Input::OPENED: {
            if (c.group || !c.joining || c.joining->code.empty()) return;
            auto j = joined_from(input.opened);
            if (!j) {
                c.joining.reset();
                cx.notice(c, NoticeKind::ReplyUnreadable, {});
                return;
            }
            for (auto& p : j->second.please_adds)
                if (p) c.names.insert(key_of(p->read_cap), sanitize(p->name));
            const Bytes& wc = input.opened.mutated_write_cap;
            c.next_write = Bytes(wc.end() - INDEX_SIZE, wc.end());
            c.write_cap = wc;
            Group g = j->first;
            Event e(Event::EPOCH_CHANGED);
            e.epoch = epoch;
            auto es = g.handle(e);
            c.group = std::move(g);
            c.joining.reset();
            c.cursors.clear();
            cx.effects(c, es);
            cx.notice(c, NoticeKind::Joined, {});
            return;
        }
        case Input::INDUCT: {
            auto voucher = from_code(input.code);
            if (!c.group) {
                cx.notice(c, NoticeKind::NotJoinedYet, {});
            } else if (c.inducting && c.inducting->payload) {
                cx.notice(c, NoticeKind::AlreadyAdding, {});
            } else if (voucher) {
                c.inducting = Inducting{};
                c.inducting->voucher = *voucher;
                Action a(Action::READ_VOUCHER);
                a.chat = id;
                a.voucher = *voucher;
                cx.emit(a);
                cx.notice(c, NoticeKind::ReadingVoucher, {});
            } else {
                cx.notice(c, NoticeKind::NotAVoucher, {});
            }
            return;
        }
        case Input::JOINER_PAYLOAD: {
            if (!c.inducting || c.inducting->payload || c.inducting->voucher != input.voucher) return;
            if (c.group) cx.enqueue(c, c.group->begin_induction(), std::nullopt, std::nullopt);
            Inducting i;
            i.voucher = input.voucher;
            i.payload = input.payload;
            i.voucher_write_cap = input.voucher_write_cap;
            i.box1 = input.box1;
            c.inducting = i;
            return;
        }
        case Input::SEALED_REPLY: {
            if (!c.inducting) return;
            auto& i = *c.inducting;
            auto& ind = input.inducted;
            i.sealing = false;
            i.sealed = Sealed{ind.name, ind.mutated_read_cap, ind.sealed_reply, ind.version};
            c.names.insert(key_of(ind.mutated_read_cap), sanitize(ind.name));
            Action publish(Action::PUBLISH);
            publish.chat = id;
            publish.voucher_write_cap = i.voucher_write_cap;
            publish.box1 = i.box1;
            publish.reply = ind.sealed_reply;
            publish.version = ind.version;
            if (c.group) {
                auto o = c.group->introduce_now(sanitize(ind.name), ind.mutated_read_cap);
                auto waiting = std::move(c.jobs);
                c.jobs.clear();
                cx.enqueue(c, {o}, std::nullopt, std::nullopt);
                c.jobs.insert(c.jobs.end(), waiting.begin(), waiting.end());
                cx.dirty(c);
            }
            cx.emit(publish);
            return;
        }
        case Input::PUBLISHED: {
            if (!c.inducting || !c.inducting->sealed) return;
            std::string who = sanitize(c.inducting->sealed->name);
            c.inducting.reset();
            cx.notice(c, NoticeKind::Added, {who});
            return;
        }
        case Input::SEND: {
            if (!c.group) {
                cx.notice(c, NoticeKind::NotJoined, {});
                return;
            }
            Bytes content = text_content(c.group->membership(nullptr), input.text, wire_version);
            Line l;
            l.sender = c.group->me();
            l.text = input.text;
            l.status = Status::Pending;
            size_t line = cx.say(c, l);
            auto outg = c.group->compose(content);
            cx.enqueue(c, outg, content, line);
            return;
        }
        case Input::SEND_FILE: {
            if (!c.group) {
                cx.notice(c, NoticeKind::NotJoined, {});
                return;
            }
            static const Bytes none;
            const Bytes& bytes = input.bytes ? *input.bytes : none;
            if (bytes.size() > max_file) {
                cx.notice(c, NoticeKind::FileTooLarge, {size_text(max_file)});
                return;
            }
            std::string safe = safe_name(input.name);
            FileUpload f{{}, filetype_of(safe), safe};
            Bytes content = file_content(c.group->membership(nullptr), f, wire_version);
            Attachment a{safe, f.filetype, bytes.size(), attachment_file(c, c.next_id, now, safe)};
            cx.keep(a.file, bytes);
            Line l;
            l.sender = c.group->me();
            l.text = file_text(a);
            l.status = Status::Pending;
            l.attachment = std::move(a);
            size_t line = cx.say(c, l);
            auto outg = c.group->compose(content);
            cx.enqueue(c, outg, content, line, &bytes);
            return;
        }
        case Input::LOADED: {
            Job* j = c.job(input.job);
            if (!j) return;
            if (input.bytes) {
                flight_.blobs[{id, input.job}] = *input.bytes;
                return;
            }
            if (j->token && c.group) c.group->abandoned(*j->token);
            c.jobs.erase(c.jobs.begin() + (j - c.jobs.data()));
            cx.notice(c, NoticeKind::SendLost, {});
            return;
        }
        case Input::SUBSTREAM: {
            Job* j = c.job(input.job);
            if (j && j->upload) {
                j->upload->write_cap = input.keypair.write_cap;
                j->upload->next = input.keypair.first;
            }
            return;
        }
        case Input::CHUNK_INDEXES: {
            Job* j = c.job(input.job);
            if (j && j->upload && j->upload->next == input.index) {
                std::vector<Index> a{input.index};
                a.insert(a.end(), input.indexes.begin(), input.indexes.end());
                flight_.ahead[{id, input.job}] = std::move(a);
            }
            return;
        }
        case Input::WROTE_CHUNK: cx.wrote_chunk(c, input.job, input.n, input.next); return;
        case Input::READ_CHUNK: cx.read_chunk(c, input.stream, input.index, input.outcome); return;
        case Input::WROTE: {
            auto it = std::find_if(c.jobs.begin(), c.jobs.end(), [&](const Job& j) { return j.id == input.job; });
            if (it == c.jobs.end()) return;
            Job j = *it;
            c.jobs.erase(it);
            if (j.token && c.group) {
                c.group->written(*j.token, input.index, Payload::raw(j.box));
                c.next_write = input.next;
            }
            if (j.line) {
                for (auto& l : c.log)
                    if (l.id == *j.line) {
                        l.status = Status::Sent;
                        l.idx64 = counter(input.index);
                        cx.line_changed(c, *j.line);
                    }
            }
            return;
        }
        case Input::REFUSED: cx.notice(c, NoticeKind::Desync, {}); return;
        case Input::READ: cx.read(c, input.stream, input.index, input.outcome); return;
        case Input::SCAN: {
            if (!c.group) return;
            for (auto& stream : c.group->readers.keys()) {
                Event e(Event::SCAN);
                e.stream = stream;
                cx.effects(c, c.group->handle(e));
            }
            cx.notice(c, NoticeKind::Scanning, {});
            return;
        }
        case Input::REMOVE_MEMBER: {
            if (!c.group || !c.group->readers.contains(input.who)) {
                cx.notice(c, NoticeKind::NoLongerMember, {});
                return;
            }
            std::string l = cx.label(c, input.who);
            c.group->remove(input.who);
            cx.stop_reading(c, input.who);
            cx.hit(c, input.who);
            flight_.fetched.erase({id, input.who});
            cx.drop_lines(c, [&](const Line& x) { return x.sender != input.who; });
            c.cursors.shift_remove(input.who);
            c.downloads.erase(std::remove_if(c.downloads.begin(), c.downloads.end(),
                                             [&](const Download& d) { return d.stream == input.who; }),
                              c.downloads.end());
            cx.notice(c, NoticeKind::StoppedReading, {l});
            return;
        }
        case Input::CLEAR: cx.drop_lines(c, [](const Line&) { return false; }); return;
        case Input::SCAN_READ: {
            if (!c.group) return;
            auto [result, next] = read_result(input.outcome);
            Event e(Event::BOX_READ);
            e.stream = input.stream;
            e.index = input.index;
            e.result = result;
            e.next = next;
            cx.effects(c, c.group->handle(e));
            cx.follow_reader(c, input.stream);
            return;
        }
        case Input::WALKED: {
            if (!c.group) return;
            Event e(Event::WALKED);
            e.stream = input.stream;
            e.walked = input.walked;
            cx.effects(c, c.group->handle(e));
            cx.follow_reader(c, input.stream);
            return;
        }
        case Input::DERIVED: {
            if (!c.group) return;
            Event e(Event::INDEXES_DERIVED);
            e.stream = input.stream;
            e.indexes = input.indexes;
            cx.effects(c, c.group->handle(e));
            return;
        }
        case Input::FAILED:
            cx.cancel_held(c);
            c.inducting.reset();
            cx.notice(c, NoticeKind::ActionFailed, {input.what});
            return;
        default: return;
    }
}

// ---- carrying actions out, as client/exec.rs ----

static const char* describe(const Action& a) {
    switch (a.kind) {
        case Action::CREATE: return "new stream";
        case Action::MINT: return "mint a voucher";
        case Action::AWAIT_REPLY: return "wait for the voucher's reply";
        case Action::READ_VOUCHER: return "read a voucher's box 0";
        case Action::SEAL: return "seal the reply to a voucher";
        case Action::PUBLISH: return "publish the reply to a voucher";
        default: return "an action";
    }
}

static Outcome outcome_of(const Read& r, const Index& index) {
    Outcome o;
    if (r.kind == Read::NOT_FOUND) return o;
    o.kind = r.kind == Read::TOMBSTONE ? Outcome::TOMBSTONE : Outcome::DATA;
    o.plaintext = r.data;
    o.next = *next_index(index);
    return o;
}

static Done done(Input i) {
    Done d;
    d.kind = Done::DONE;
    d.input = std::move(i);
    return d;
}

Backend::Write Executor::publish(const Bytes& write_cap, const Index& index, const Bytes& b) {
    for (;;) {
        auto w = backend.write_box(write_cap, index, b);
        if (w != Backend::Write::FAILED) return w;
        sleep(RETRY_MS);
    }
}

Done Executor::perform(const Action& a) {
    if (auto d = perform_or(a)) return *d;
    switch (a.kind) {
        case Action::DELETE: return Done{};
        case Action::READ: case Action::SCAN_READ: case Action::WRITE: case Action::DERIVE: case Action::WALK:
        case Action::READ_CHUNK: case Action::SUBSTREAM: case Action::CHUNK_INDEXES: case Action::WRITE_CHUNK: {
            Done d;
            d.kind = Done::RETRY;
            d.action = a;
            d.ms = RETRY_MS;
            return d;
        }
        default: {
            Input i(Input::FAILED);
            i.chat = a.chat;
            i.what = std::string(describe(a)) + " failed: " + backend.error();
            return done(i);
        }
    }
}

std::optional<Done> Executor::perform_or(const Action& a) {
    Backend& b = backend;
    switch (a.kind) {
        case Action::CREATE: {
            auto kp = b.new_keypair();
            if (!kp) return std::nullopt;
            Input i(Input::CREATED);
            i.chat = a.chat;
            i.joining = a.joining;
            i.keypair = *kp;
            return done(i);
        }
        case Action::WRITE: {
            auto w = b.write_box(a.write_cap, a.index, a.box);
            if (w == Backend::Write::FAILED) return std::nullopt;
            Input i(w == Backend::Write::EXISTS ? Input::REFUSED : Input::WROTE);
            i.chat = a.chat;
            i.job = a.job;
            i.index = a.index;
            i.next = *next_index(a.index);
            return done(i);
        }
        case Action::READ:
        case Action::SCAN_READ: {
            auto r = b.read_box(a.read_cap, a.index);
            if (!r) return std::nullopt;
            Input i(a.kind == Action::READ ? Input::READ : Input::SCAN_READ);
            i.chat = a.chat;
            i.stream = a.stream;
            i.index = a.index;
            i.outcome = outcome_of(*r, a.index);
            return done(i);
        }
        case Action::DELETE:
            if (!files.remove) return std::nullopt;
            files.remove(a.file);
            return Done{};
        case Action::LOAD: {
            Input i(Input::LOADED);
            i.chat = a.chat;
            i.job = a.job;
            if (files.read) i.bytes = files.read(a.file);
            return done(i);
        }
        case Action::SUBSTREAM: {
            auto kp = b.new_keypair();
            if (!kp) return std::nullopt;
            Input i(Input::SUBSTREAM);
            i.chat = a.chat;
            i.job = a.job;
            i.keypair = *kp;
            return done(i);
        }
        case Action::WRITE_CHUNK: {
            auto w = b.write_box(a.write_cap, a.index, a.box);
            if (w == Backend::Write::FAILED) return std::nullopt;
            Input i(w == Backend::Write::EXISTS ? Input::REFUSED : Input::WROTE_CHUNK);
            i.chat = a.chat;
            i.job = a.job;
            i.n = a.n;
            i.next = *next_index(a.index);
            return done(i);
        }
        case Action::CHUNK_INDEXES: {
            auto is = derive_with(a.start, a.count);
            if (!is) return std::nullopt;
            Input i(Input::CHUNK_INDEXES);
            i.chat = a.chat;
            i.job = a.job;
            i.index = a.start;
            i.indexes = *is;
            return done(i);
        }
        case Action::READ_CHUNK: {
            auto r = b.read_box(a.read_cap, a.index);
            if (!r) return std::nullopt;
            Input i(Input::READ_CHUNK);
            i.chat = a.chat;
            i.stream = a.stream;
            i.index = a.index;
            i.outcome = outcome_of(*r, a.index);
            return done(i);
        }
        case Action::WAKE: {
            Input i(Input::WAKE);
            i.chat = a.chat;
            i.stream = a.stream;
            Done d;
            d.kind = Done::LATER;
            d.input = i;
            d.ms = a.ms;
            return d;
        }
        case Action::WALK: {
            auto w = walk_with(a.start, a.targets, a.keep);
            if (!w) return std::nullopt;
            Input i(Input::WALKED);
            i.chat = a.chat;
            i.stream = a.stream;
            i.walked = *w;
            return done(i);
        }
        case Action::DERIVE: {
            auto is = derive_with(a.start, a.count);
            if (!is) return std::nullopt;
            Input i(Input::DERIVED);
            i.chat = a.chat;
            i.stream = a.stream;
            i.indexes = *is;
            return done(i);
        }
        case Action::MINT: {
            auto m = b.voucher_mint(a.write_cap, a.name, a.form);
            if (!m) return std::nullopt;
            Index box0(m->write_cap.end() - INDEX_SIZE, m->write_cap.end());
            if (publish(m->write_cap, box0, m->payload) != Backend::Write::OK) return std::nullopt;
            Input i(Input::MINTED);
            i.chat = a.chat;
            i.minted = *m;
            i.box1 = *next_index(box0);
            return done(i);
        }
        case Action::AWAIT_REPLY: {
            Bytes sealed;
            Index index = a.box1;
            for (;;) {
                auto r = b.read_box(a.voucher_read_cap, index);
                if (!r || r->kind == Read::NOT_FOUND) {
                    Done d;
                    d.kind = Done::RETRY;
                    d.action = a;
                    d.ms = RETRY_MS;
                    return d;
                }
                if (r->kind == Read::TOMBSTONE) {
                    Input i(Input::FAILED);
                    i.chat = a.chat;
                    i.what = "the voucher's reply was withdrawn";
                    return done(i);
                }
                const Bytes& piece = r->data;
                if (a.version < 2) {
                    sealed = piece;
                } else {
                    if (piece.empty() || (piece[0] != 'C' && piece[0] != 'F')) {
                        Input i(Input::FAILED);
                        i.chat = a.chat;
                        i.what = "the voucher's reply is not framed";
                        return done(i);
                    }
                    sealed.insert(sealed.end(), piece.begin() + 1, piece.end());
                    if (piece[0] == 'C') {
                        auto n = next_index(index);
                        if (!n) return std::nullopt;
                        index = *n;
                        continue;
                    }
                }
                auto o = b.voucher_open(a.secret_key, sealed, a.write_cap);
                if (!o) return std::nullopt;
                Input i(Input::OPENED);
                i.chat = a.chat;
                i.opened = *o;
                return done(i);
            }
        }
        case Action::READ_VOUCHER: {
            Done retry;
            retry.kind = Done::RETRY;
            retry.action = a;
            retry.ms = RETRY_MS;
            auto s = b.voucher_derive_stream(a.voucher);
            if (!s) return retry;
            Index box0 = start_of(s->second);
            auto r = b.read_box(s->second, box0);
            if (!r || r->kind == Read::NOT_FOUND) return retry;
            if (r->kind == Read::TOMBSTONE) {
                Input i(Input::FAILED);
                i.chat = a.chat;
                i.what = "that voucher was withdrawn";
                return done(i);
            }
            Input i(Input::JOINER_PAYLOAD);
            i.chat = a.chat;
            i.voucher = a.voucher;
            i.voucher_write_cap = s->first;
            i.payload = r->data;
            i.box1 = *next_index(box0);
            return done(i);
        }
        case Action::SEAL: {
            auto ind = b.voucher_induct(a.voucher, a.payload, a.who_reply);
            if (!ind) return std::nullopt;
            Input i(Input::SEALED_REPLY);
            i.chat = a.chat;
            i.inducted = *ind;
            return done(i);
        }
        case Action::PUBLISH: {
            Input pub(Input::PUBLISHED);
            pub.chat = a.chat;
            if (a.version < 2) {
                if (publish(a.voucher_write_cap, a.box1, a.reply) != Backend::Write::OK) return std::nullopt;
                return done(pub);
            }
            auto max = b.max_plaintext();
            if (!max) return std::nullopt;
            size_t piece = std::max<size_t>(*max > 0 ? *max - 1 : 0, 1);
            Index index = a.box1;
            size_t off = 0;
            for (size_t n = 0;; n++) {
                size_t len = std::min(piece, a.reply.size() - off);
                bool last = a.reply.size() - off <= piece;
                Bytes box{(uint8_t)(last ? 'F' : 'C')};
                box.insert(box.end(), a.reply.begin() + off, a.reply.begin() + off + len);
                off += len;
                auto w = publish(a.voucher_write_cap, index, box);
                if (w == Backend::Write::EXISTS && n == 0) return std::nullopt;  // another member answered
                if (last) return done(pub);
                auto nx = next_index(index);
                if (!nx) return std::nullopt;
                index = *nx;
            }
        }
    }
    return std::nullopt;
}

}  // namespace kq1
