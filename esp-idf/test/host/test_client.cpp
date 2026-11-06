// SPDX-License-Identifier: AGPL-3.0-only
// The katzenqt1 engine and executor against group_chat_client's client scenarios, through an
// in-memory transport, as pigeonhole_protocols rust/tests/katzenqt1_client_scenarios.rs and its
// harness (tests/katzenqt1_client_harness) with transport/sim.rs. The scenarios are read from
// KQ1_TESTDATA, a pigeonhole_protocols checkout's testdata/katzenqt1.
#include <sodium.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "kq1_engine.h"
#include "kq_ogg.h"
#include "kq_radio_feed.h"
#include "kq_store.h"
#include "vectors.h"

using namespace kq1;
namespace fs = std::filesystem;

// ---- transport/sim.rs ----

static Keypair keypair_from_seed(const uint8_t seed[32], const uint8_t index_seed[48]) {
    Bytes pk(32), sk(64);
    crypto_sign_ed25519_seed_keypair(pk.data(), sk.data(), seed);
    uint8_t r[16];
    memcpy(r, index_seed + 32, 16);
    r[7] &= 0x3f;
    r[15] &= 0x3f;
    uint64_t a = 0, b = 0;
    for (int i = 7; i >= 0; i--) { a = a << 8 | r[i]; b = b << 8 | r[8 + i]; }
    uint64_t idx = a + b;
    Bytes index(8);
    for (int i = 0; i < 8; i++) index[i] = (uint8_t)(idx >> (8 * i));
    index.insert(index.end(), 64, 0);
    index.insert(index.end(), index_seed, index_seed + 32);
    Keypair kp;
    kp.first = *next_index(index);
    kp.read_cap = pk;
    kp.read_cap.insert(kp.read_cap.end(), kp.first.begin(), kp.first.end());
    kp.write_cap.assign(seed, seed + 32);
    kp.write_cap.insert(kp.write_cap.end(), kp.read_cap.begin(), kp.read_cap.end());
    return kp;
}

static Keypair voucher_stream(const Bytes& voucher) {
    static const char info[] = "sim voucher";
    uint8_t seed[80];
    kq::hkdf_blake2b(64, voucher.data(), voucher.size(), nullptr, 0, (const uint8_t*)info, sizeof info - 1, seed, 80);
    return keypair_from_seed(seed, seed + 32);
}

struct Sim : Backend {
    std::map<std::pair<Bytes, Bytes>, std::pair<uint64_t, Bytes>> boxes;
    uint64_t epoch = 1;
    size_t max = 1530;
    size_t fail_writes = 0;
    std::vector<std::pair<Bytes, uint64_t>> refused;

    static std::optional<Bytes> read_cap_of(const Bytes& wc) {
        if (wc.size() != 32 + 136) return std::nullopt;
        return Bytes(wc.begin() + 32, wc.end());
    }
    std::optional<Keypair> new_keypair() override {
        uint8_t b[80];
        randombytes_buf(b, sizeof b);
        return keypair_from_seed(b, b + 32);
    }
    Write write_box(const Bytes& write_cap, const Index& index, const Bytes& plaintext) override {
        if (fail_writes > 0) { fail_writes--; return Write::FAILED; }
        auto rc = read_cap_of(write_cap);
        if (!rc || plaintext.size() > max) return Write::FAILED;
        auto k = std::make_pair(Bytes(rc->begin(), rc->begin() + 32), index);
        auto it = boxes.find(k);
        if (it != boxes.end() && it->second.first + 1 >= epoch && !plaintext.empty()) {
            if (it->second.second == plaintext) return Write::OK;
            refused.push_back({k.first, counter(index)});
            return Write::EXISTS;
        }
        boxes[k] = {epoch, plaintext};
        return Write::OK;
    }
    std::optional<Read> read_box(const Bytes& read_cap, const Index& index) override {
        auto it = boxes.find({Bytes(read_cap.begin(), read_cap.begin() + 32), index});
        Read r;
        if (it != boxes.end() && it->second.first + 1 >= epoch) {
            r.kind = it->second.second.empty() ? Read::TOMBSTONE : Read::DATA;
            r.data = it->second.second;
        }
        return r;
    }
    std::optional<size_t> max_plaintext() override { return max; }
    std::optional<Minted> voucher_mint(const Bytes& write_cap, const std::string& name, uint8_t form) override {
        Bytes v(32);
        randombytes_buf(v.data(), 32);
        auto kp = voucher_stream(v);
        auto rc = read_cap_of(write_cap);
        if (!rc) return std::nullopt;
        Bytes payload;
        if (form == 1) payload.push_back(0xa0);
        payload.insert(payload.end(), name.begin(), name.end());
        payload.push_back(0);
        payload.insert(payload.end(), rc->begin(), rc->end());
        Minted m;
        m.version = voucher_version(payload);
        m.voucher = v;
        m.payload = payload;
        m.write_cap = kp.write_cap;
        m.read_cap = kp.read_cap;
        m.secret_key = v;
        return m;
    }
    std::optional<std::pair<Bytes, Bytes>> voucher_derive_stream(const Bytes& voucher) override {
        auto kp = voucher_stream(voucher);
        return std::make_pair(kp.write_cap, kp.read_cap);
    }
    std::optional<Inducted> voucher_induct(const Bytes&, const Bytes& payload, const Bytes& who_reply) override {
        Inducted i;
        i.version = voucher_version(payload);
        Bytes p = payload;
        if (!p.empty() && p[0] == 0xa0) p.erase(p.begin());
        auto z = std::find(p.begin(), p.end(), 0);
        if (z == p.end()) return std::nullopt;
        i.name.assign(p.begin(), z);
        i.mutated_read_cap.assign(z + 1, p.end());
        i.sealed_reply = who_reply;
        return i;
    }
    std::optional<Opened> voucher_open(const Bytes&, const Bytes& sealed, const Bytes& write_cap) override {
        return Opened{sealed, write_cap};
    }
};

// ---- the harness ----

static std::string kind_of(const Action& a) {
    static const char* names[] = {"Create", "Write", "Read", "Wake", "ScanRead", "Walk", "Derive", "Mint", "AwaitReply",
                                  "ReadVoucher", "Seal", "Publish", "Delete", "Load", "Substream", "WriteChunk",
                                  "ChunkIndexes", "ReadChunk"};
    return names[a.kind];
}

struct Client {
    std::string name;
    Engine eng;
    // What it saves after every step, which must load back as the state it has.
    std::shared_ptr<std::map<std::string, std::string>> files = std::make_shared<std::map<std::string, std::string>>();
    std::shared_ptr<kq::Store> store = std::make_shared<kq::Store>(kq::memory_files(*files));
    std::shared_ptr<std::map<std::string, Bytes>> kept = std::make_shared<std::map<std::string, Bytes>>();  // its files
    std::vector<Clip> clips;  // radio bursts handed out to play
    std::vector<Ping> pings;
};

static int g_saved = 0, g_unsaved = 0;

struct Later {
    uint64_t due;
    size_t who;
    std::optional<Action> act;
    std::optional<Input> in;
};

struct World {
    Sim sim;
    std::vector<Client> members;
    std::vector<std::pair<size_t, Action>> queue;
    std::vector<Later> later;
    uint64_t clock = 0, epoch = 1;
    std::set<size_t> held;
    std::map<size_t, std::string> delay_next;
    std::map<size_t, std::vector<Input>> delayed;
    std::map<std::string, size_t> performed;  // actions carried out, by kind

    size_t who(const std::string& name) {
        for (size_t i = 0; i < members.size(); i++) if (members[i].name == name) return i;
        members.push_back({name, Engine(State{name, {}}, 1530)});
        return members.size() - 1;
    }
    void feed(size_t i, const Input& input) { release(i, members[i].eng.step(0, input)); }
    void release(size_t i, const StepResult& res) {
        Client& m = members[i];
        if (!m.store->commit(m.eng.state, res.changes)) throw std::runtime_error("store: a write failed");
        std::vector<std::string> problems;
        kq::Store fresh(kq::memory_files(*m.files));
        bool same = kq::state_dump(fresh.load(m.name, &problems)) == kq::state_dump(m.eng.state) && problems.empty();
        (same ? g_saved : g_unsaved)++;
        if (!same) fprintf(stderr, "%s: the saved state differs from the state after a step\n", m.name.c_str());
        for (auto& f : res.files) (*m.kept)[f.file] = f.bytes;
        for (auto& c : res.clips) m.clips.push_back(c);
        for (auto& p : res.pings) m.pings.push_back(p);
        for (auto& a : res.actions) queue.push_back({i, a});
    }
    bool act() {
        auto it = std::find_if(queue.begin(), queue.end(), [&](auto& q) { return !held.count(q.first); });
        if (it == queue.end()) return false;
        auto [w, a] = *it;
        queue.erase(it);
        auto kept = members[w].kept;
        Executor x{sim, [](uint64_t) {}, {[kept](const std::string& f) -> std::optional<Bytes> {
                                              auto it = kept->find(f);
                                              return it == kept->end() ? std::nullopt : std::optional<Bytes>(it->second);
                                          },
                                          [kept](const std::string& f) { kept->erase(f); }}};
        std::string kind = kind_of(a);
        performed[kind]++;
        Done o = x.perform(a);
        if (o.kind == Done::DONE && delay_next.count(w) && delay_next[w] == kind) {
            delay_next.erase(w);
            delayed[w].push_back(*o.input);
        } else if (o.kind == Done::RETRY) {
            later.push_back({clock + o.ms, w, *o.action, std::nullopt});
        } else if (o.kind == Done::LATER) {
            later.push_back({clock + o.ms, w, std::nullopt, *o.input});
        } else if (o.kind == Done::DONE) {
            feed(w, *o.input);
        }
        return true;
    }
    void run_to(uint64_t stop) {
        for (;;) {
            if (act()) continue;
            std::optional<uint64_t> due;
            for (auto& l : later)
                if (l.due <= stop && !held.count(l.who) && (!due || l.due < *due)) due = l.due;
            if (!due) {
                clock = std::max(clock, stop);
                return;
            }
            std::vector<Later> now, rest;
            for (auto& l : later) (l.due <= *due && !held.count(l.who) ? now : rest).push_back(l);
            later = rest;
            clock = std::max(clock, *due);
            for (auto& l : now) {
                if (l.act) {
                    if (members[l.who].eng.state.wanted(*l.act)) queue.push_back({l.who, *l.act});
                } else {
                    feed(l.who, *l.in);
                }
            }
        }
    }
    const Chat* chat(size_t i, const std::string& g) const { return members[i].eng.state.chat(g); }
    std::string name_of(const Key& k) const {
        for (auto& m : members)
            for (auto& c : m.eng.state.chats)
                if (c.group && c.group->me() == k) return m.name;
        return "?";
    }
    std::string code(size_t i, const std::string& g) const {
        auto c = chat(i, g);
        return c && c->joining ? c->joining->code : "";
    }
    static uint64_t first_of(const Chat& c) { return counter(Bytes(c.write_cap.end() - INDEX_SIZE, c.write_cap.end())); }
    std::vector<std::string> show(size_t i, const std::string& g) const {
        auto c = chat(i, g);
        if (!c) return {"no such group"};
        std::vector<std::string> out;
        for (auto& l : c->log) {
            if (l.notice) { out.push_back("— " + plain(*l.notice)); continue; }
            bool mine = c->group && c->group->me() == l.sender;
            std::string mark = !mine ? "" : l.status == Status::Pending ? " …" : c->acknowledged(l) ? " ✓✓" : " ✓";
            std::string text = l.text;
            for (size_t p; (p = text.find('\n')) != std::string::npos;) text.replace(p, 1, " | ");
            out.push_back(name_of(l.sender) + mark + ": " + text);
        }
        return out;
    }
    std::vector<std::string> members_of(size_t i, const std::string& g) const {
        auto c = chat(i, g);
        if (!c) return {"no such group"};
        if (!c->group) return {"not joined"};
        std::vector<std::string> ns;
        for (auto& [k, _] : c->group->readers) ns.push_back(name_of(k));
        std::sort(ns.begin(), ns.end());
        std::string s;
        for (auto& n : ns) s += (s.empty() ? "" : " ") + n;
        return {s.empty() ? "nobody" : s};
    }
    std::vector<std::string> acked(size_t i, const std::string& g) const {
        auto c = chat(i, g);
        if (!c) return {"no such group"};
        if (!c->group) return {"not joined"};
        uint64_t first = first_of(*c);
        std::vector<std::string> as;
        for (auto& [k, n] : c->group->ledger.acked_by) as.push_back(name_of(k) + ":" + std::to_string(n - first));
        std::sort(as.begin(), as.end());
        std::string s;
        for (auto& x : as) s += (s.empty() ? "" : " ") + x;
        return {s.empty() ? "none" : s};
    }
    std::vector<std::string> state_of(size_t i, const std::string& g) const {
        auto c = chat(i, g);
        if (!c) return {"no such group"};
        std::string s = c->group ? "joined" : "not joined";
        if (c->joining) s += c->joining->code.empty() ? ", minting" : ", awaiting a reply";
        if (c->inducting) s += ", adding a member";
        if (!c->jobs.empty()) s += ", " + std::to_string(c->jobs.size()) + " to write";
        return {s};
    }
    std::vector<std::string> jobs(size_t i, const std::string& g) const {
        auto c = chat(i, g);
        if (!c) return {"no such group"};
        if (c->jobs.empty()) return {"none"};
        uint64_t first = first_of(*c);
        std::vector<std::string> out;
        for (auto& j : c->jobs) {
            std::string at = j.index ? "box " + std::to_string(counter(*j.index) - first) : "next box";
            std::string what = j.token ? "message" : j.box.empty() ? "tombstone" : "rewrite";
            out.push_back(what + " to " + at + ", " + std::to_string(j.box.size()) + " bytes");
        }
        return out;
    }
    std::vector<std::string> refused_names() const {
        std::vector<std::string> names;
        for (auto& [stream, idx64] : sim.refused) {
            std::string name = "?";
            uint64_t n = idx64;
            for (auto& m : members)
                for (auto& c : m.eng.state.chats)
                    if (c.write_cap.size() > 64 && Bytes(c.write_cap.begin() + 32, c.write_cap.begin() + 64) == stream) {
                        name = m.name;
                        n = idx64 - first_of(c);
                    }
            std::string s = name + "@" + std::to_string(n);
            if (std::find(names.begin(), names.end(), s) == names.end()) names.push_back(s);
        }
        std::string s;
        for (auto& x : names) s += (s.empty() ? "" : " ") + x;
        return {s.empty() ? "none" : s};
    }
    std::vector<std::string> command(const std::vector<std::string>& w) {
        auto is = [&](const char* c, size_t n) { return w.size() == n && w[0] == c; };
        auto in = [&](Input::Kind k, const std::string& g) {
            Input x(k);
            x.chat = g;
            return x;
        };
        if (is("create", 3) || is("join", 3)) {
            Input x = in(w[0] == "create" ? Input::CREATE : Input::JOIN, w[2]);
            x.name = w[2];
            feed(who(w[1]), x);
        } else if (w.size() >= 4 && w[0] == "invite") {
            std::string theirs = w.size() > 4 ? w[4] : w[2];
            size_t n = who(w[3]);
            std::string c = code(n, theirs);
            if (c.empty()) return {w[3] + " has no voucher code"};
            Input x = in(Input::INDUCT, w[2]);
            x.code = c;
            feed(who(w[1]), x);
        } else if (w.size() >= 3 && w[0] == "send") {
            Input x = in(Input::SEND, w[2]);
            for (size_t k = 3; k < w.size(); k++) x.text += (k > 3 ? " " : "") + w[k];
            feed(who(w[1]), x);
        } else if (is("remove", 4)) {
            size_t n = who(w[3]);
            auto c = chat(n, w[2]);
            if (!c || !c->group) return {w[3] + " is not in " + w[2]};
            Input x = in(Input::REMOVE_MEMBER, w[2]);
            x.who = c->group->me();
            feed(who(w[1]), x);
        } else if (is("scan", 3)) {
            feed(who(w[1]), in(Input::SCAN, w[2]));
        } else if (is("leave", 3)) {
            feed(who(w[1]), in(Input::LEAVE, w[2]));
        } else if (is("run", 1)) {
            run_to(clock + 600000);
        } else if (is("run", 2)) {
            run_to(clock + std::stoull(w[1]) * 1000);
        } else if (is("hold", 2)) {
            held.insert(who(w[1]));
        } else if (is("release", 2)) {
            size_t i = who(w[1]);
            held.erase(i);
            auto ds = delayed[i];
            delayed.erase(i);
            for (auto& x : ds) feed(i, x);
        } else if (is("delay-next", 3)) {
            delay_next[who(w[1])] = w[2];
        } else if (is("epoch", 1)) {
            epoch++;
            sim.epoch = epoch;
            for (size_t i = 0; i < members.size(); i++) {
                Input x(Input::EPOCH);
                x.n = epoch;
                feed(i, x);
            }
        } else if (is("fail-writes", 2)) {
            sim.fail_writes = std::stoul(w[1]);
        } else if (is("show", 3)) {
            return show(who(w[1]), w[2]);
        } else if (is("members", 3)) {
            return members_of(who(w[1]), w[2]);
        } else if (is("acked", 3)) {
            return acked(who(w[1]), w[2]);
        } else if (is("state", 3)) {
            return state_of(who(w[1]), w[2]);
        } else if (is("jobs", 3)) {
            return jobs(who(w[1]), w[2]);
        } else if (is("refused", 1)) {
            return refused_names();
        } else {
            std::string all;
            for (auto& x : w) all += x + " ";
            throw std::runtime_error("not a command: " + all);
        }
        return {};
    }
};

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string run_script(const std::string& script) {
    World world;
    std::vector<std::string> lines;
    size_t p = 0, q;
    while ((q = script.find('\n', p)) != std::string::npos) { lines.push_back(script.substr(p, q - p)); p = q + 1; }
    if (p < script.size()) lines.push_back(script.substr(p));
    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size();) {
        out.push_back(lines[i]);
        std::string cmd = trim(lines[i].substr(0, lines[i].find('#')));
        i++;
        if (cmd.empty()) continue;
        std::vector<std::string> words;
        std::istringstream ws(cmd);
        for (std::string x; ws >> x;) words.push_back(x);
        auto printed = world.command(words);
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

// ---- files, as rust/tests/katzenqt1_client_core.rs ----

static Bytes bytes_of(size_t n, size_t seed) {
    Bytes b(n);
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((i * 31 + seed) % 251);
    return b;
}

static void found_and_join(World& w) {
    w.command({"create", "alice", "g"});
    w.command({"join", "bob", "g"});
    w.run_to(w.clock + 600000);
    w.command({"invite", "alice", "g", "bob"});
    w.run_to(w.clock + 600000);
}

static void send_file(World& w, size_t i, const std::string& name, Bytes bytes) {
    Input x(Input::SEND_FILE);
    x.chat = "g";
    x.name = name;
    x.bytes = std::move(bytes);
    w.feed(i, x);
}

static const Attachment* attachment(const World& w, size_t i, const std::string& name) {
    auto c = w.chat(i, "g");
    if (c) for (auto& l : c->log) if (l.attachment && l.attachment->name == name) return &*l.attachment;
    return nullptr;
}

static std::optional<Bytes> file(const World& w, size_t i, const std::string& name) {
    auto a = attachment(w, i, name);
    if (!a) return std::nullopt;
    auto it = w.members[i].kept->find(a->file);
    return it == w.members[i].kept->end() ? std::nullopt : std::optional<Bytes>(it->second);
}

static bool says(const World& w, size_t i, const std::string& text) {
    auto c = w.chat(i, "g");
    if (c) for (auto& l : c->log) if (!l.notice && l.text == text) return true;
    return false;
}

static bool noticed(const World& w, size_t i, const std::string& text) {
    auto c = w.chat(i, "g");
    if (c) for (auto& l : c->log) if (l.notice && plain(*l.notice).find(text) != std::string::npos) return true;
    return false;
}

static size_t spooled(const World& w, size_t i) {
    size_t n = 0;
    for (auto& [f, _] : *w.members[i].kept) n += f.compare(0, 6, "spool/") == 0;
    return n;
}

// The boxes of the substream an I box in alice's stream names, in order, as the sim holds them.
static std::vector<Bytes> substream_of(const World& w, const Bytes& ibox) {
    auto ch = unframe(ibox);
    std::vector<Bytes> out;
    Bytes key(ch.data.begin(), ch.data.begin() + KEY_SIZE);
    Index at = start_of(ch.data);
    for (size_t n = 0; n < ch.total.value_or(0); n++) {
        auto it = w.sim.boxes.find({key, at});
        if (it == w.sim.boxes.end()) return {};
        out.push_back(it->second.second);
        at = *next_index(at);
    }
    return out;
}

// What the C++ engine wrote for an upload, for pigeonhole_protocols to read (filevec read).
static std::vector<Json> g_dump;
static void dump_upload(const World& w, const std::string& what, const std::string& type, const std::string& name,
                        const Bytes& payload, size_t want) {
    Vec v{"client", "files", {}};
    Bytes ibox;
    for (auto& [k, b] : w.sim.boxes)
        if (!b.second.empty() && b.second[0] == 'I') ibox = b.second;
    auto chunks = substream_of(w, ibox);
    check(chunks.size() == want, v, "the substream as written");
    std::vector<Json> cs;
    for (auto& c : chunks) cs.push_back(Json::str(tohex(c)));
    g_dump.push_back(Json::obj({{"name", Json::str(what)}, {"filetype", Json::str(type)}, {"basename", Json::str(name)},
                                {"payload", Json::str(tohex(payload))}, {"chunks", Json::arr(cs)},
                                {"indirection", Json::str(tohex(ibox))}, {"read_cap", Json::str(tohex(unframe(ibox).data))}}));
}

// A burst sent as it is made: started, its bytes given piece by piece with time passing between
// them, ended. How many chunks went out while it was made, and how many after its end.
struct Streamed { size_t during = 0, after = 0; };
static Streamed stream_send(World& w, size_t i, const std::string& name, const Bytes& payload, size_t piece,
                            uint64_t gap_ms) {
    auto in = [&](Input::Kind k) {
        Input x(k);
        x.chat = "g";
        x.name = name;
        return x;
    };
    w.feed(i, in(Input::STREAM_START));
    size_t before = w.performed["WriteChunk"];
    for (size_t at = 0; at < payload.size(); at += piece) {
        Input d = in(Input::STREAM_DATA);
        d.bytes = Bytes(payload.begin() + at, payload.begin() + std::min(at + piece, payload.size()));
        w.feed(i, d);
        w.run_to(w.clock + gap_ms);
    }
    Streamed r;
    r.during = w.performed["WriteChunk"] - before;
    w.feed(i, in(Input::STREAM_END));
    before = w.performed["WriteChunk"];
    w.run_to(w.clock + 60000);
    r.after = w.performed["WriteChunk"] - before;
    return r;
}

// The substream boxes of the upload in the sim that carries this payload.
static std::vector<Bytes> upload_of(const World& w, const Bytes& payload) {
    for (auto& [k, b] : w.sim.boxes) {
        if (b.second.empty() || b.second[0] != 'I') continue;
        auto cs = substream_of(w, b.second);
        auto m = assemble(cs);
        auto t = m ? take_payload(*m) : std::nullopt;
        if (t && t->second == payload) return cs;
    }
    return {};
}

static void file_tests(const char* out_path) {
    Vec v{"client", "files", {}};
    {
        World w;
        found_and_join(w);
        std::string text;
        for (int i = 0; i < 400; i++) text += "a long message ";
        Input x(Input::SEND);
        x.chat = "g";
        x.text = text;
        w.feed(0, x);
        x.text = "after it";
        w.feed(0, x);
        w.run_to(w.clock + 600000);
        auto& log = w.chat(1, "g")->log;
        check(log.size() >= 2 && log[log.size() - 2].text == text && log.back().text == "after it", v,
              "a long message, then one after it, in order");
    }
    {
        World w;
        found_and_join(w);
        Bytes pic = bytes_of(20000, 7);
        send_file(w, 0, "cat.png", pic);
        bool pending = false;
        for (auto& l : w.chat(0, "g")->log) pending |= l.attachment && l.status == Status::Pending;
        check(pending && spooled(w, 0) == 1, v, "a file sent shows pending, its message spooled");
        w.run_to(w.clock + 600000);
        auto a = attachment(w, 1, "cat.png");
        check(a && a->filetype == "image/png" && a->size == 20000 && file(w, 1, "cat.png") == pic, v,
              "an image in 14 chunks arrives whole");
        check(file(w, 0, "cat.png") == pic && w.chat(0, "g")->jobs.empty() && spooled(w, 0) == 0, v,
              "the sender keeps its copy, and the spool is emptied");
        check(says(w, 1, "[file cat.png, 20 KB]"), v, "the line names it");
        // A radio group is polled every second however long it has been quiet; the least backoff is 2 s.
        w.members[1].eng.radio.insert("g");
        w.run_to(w.clock + 600000);
        uint64_t bob = 0;
        for (auto& l : w.later)
            if (l.in && l.in->kind == Input::WAKE && l.who == 1) bob = std::max(bob, l.due - w.clock);
        check(bob == RADIO_POLL_MS && backoff(0) > RADIO_POLL_MS, v, "radio polls every second");
        if (out_path) dump_upload(w, "C++ engine: cat.png in 14 chunks", "image/png", "cat.png", pic, 14);
    }
    {
        // A burst of ours, recorded from lxst's batches and sent as hushphone sends a clip.
        World w;
        found_and_join(w);
        std::ifstream in("vectors/hushphone/tone.opus", std::ios::binary);
        Bytes tone((std::istreambuf_iterator<char>(in)), {});
        auto o = kq::ogg::demux(tone.data(), tone.size());
        kq::radio::Recorder rec(3);
        for (auto& p : o->packets) {
            Bytes m{kq::radio::kEvBatch, 1, (uint8_t)(p.size() + 1), 0, kq::radio::kCodecOpus};
            m.insert(m.end(), p.begin(), p.end());
            rec.batch(m.data(), m.size());
        }
        rec.end();
        Bytes clip = rec.take();
        uint8_t r32[32] = {1};
        std::string name = kq::radio::burst_name(r32);
        send_file(w, 0, name, clip);
        w.run_to(w.clock + 600000);
        auto a = attachment(w, 1, name);
        check(a && a->filetype == "audio/opus" && file(w, 1, name) == clip, v, "a recorded burst arrives as a voice clip");
        if (out_path) dump_upload(w, "C++ engine: a recorded burst", "audio/opus", name, clip, 2);
    }
    {
        // A burst streamed while it is made: its chunks go out as they fill, so that after the talk
        // key is let go only the first (with the length) and the last are left to write.
        World w;
        found_and_join(w);
        for (auto& m : w.members) m.eng.radio.insert("g");
        std::ifstream in("vectors/hushphone/tone.opus", std::ios::binary);
        Bytes tone((std::istreambuf_iterator<char>(in)), {});
        auto o = kq::ogg::demux(tone.data(), tone.size());
        kq::ogg::Opus twelve = *o;
        for (int k = 0; k < 11; k++) twelve.packets.insert(twelve.packets.end(), o->packets.begin(), o->packets.end());
        Bytes clip = kq::ogg::mux(twelve, 4);  // 12 s, about 18 KB: 13 chunks
        auto r = stream_send(w, 0, "aa.opus", clip, 1550, 1000);
        auto& bob = w.members[1];
        check(bob.clips.size() == 1 && bob.clips[0].bytes == clip && bob.clips[0].name == "aa.opus", v,
              "a streamed burst is heard whole");
        check(r.during >= 10 && r.after <= 2, v, "its chunks went out while it was made, two after its end");
        if (out_path) dump_upload(w, "C++ engine: a burst streamed as it was made", "audio/opus", "aa.opus", clip, 13);
        bool saved = false;
        std::string raw(clip.begin() + 5000, clip.begin() + 5040);
        for (auto& [_, d] : *w.members[0].files) saved |= d.find(raw) != std::string::npos || d.find(tohex(Bytes(raw.begin(), raw.end()))) != std::string::npos;
        check(!saved && w.chat(0, "g")->jobs.empty(), v, "and none of it was saved");

        // Where its chunks end: one chunk, two, a message ending on a chunk's edge, the 2-byte and
        // 4-byte payload lengths' edges, and the most a burst lasts.
        size_t box = 1530 - 1, edgeDone = 0;
        auto exact = [&](const Bytes& p, const std::string& name) {
            bob.clips.clear();
            stream_send(w, 0, name, p, 4000, 300);
            return bob.clips.size() == 1 && bob.clips[0].bytes == p;
        };
        bool ok = true;
        for (size_t n : {size_t(10), size_t(1400), size_t(2000), size_t(65535), size_t(65536), size_t(108000)})
            ok &= exact(bytes_of(n, n), "n" + std::to_string(n) + ".opus");
        check(ok, v, "1 and 2 chunks, and payloads of 65535, 65536 and 108000 bytes");
        // A message of exactly two chunks: its size found from one streamed before it.
        {
            size_t overhead = 0;
            for (auto& c : upload_of(w, bytes_of(108000, 108000))) overhead += c.size() - 1;
            overhead -= 108000;
            bool all = overhead > 0 && overhead < 400;
            for (size_t n = 2 * box - overhead - 2; all && n <= 2 * box - overhead + 2; n++) {
                Bytes p = bytes_of(n, 1);
                all &= exact(p, "e" + std::to_string(n) + ".opus");
                size_t sz = 0;
                auto cs = upload_of(w, p);
                for (auto& c : cs) sz += c.size() - 1;
                if (sz == 2 * box && cs.size() == 2 && cs[1][0] == 'F') edgeDone++;
            }
            check(all && edgeDone >= 1, v, "a message exactly two chunks long");
        }

        // A restart while it is made: it is lost, says so, and the next one goes.
        Input st(Input::STREAM_START);
        st.chat = "g";
        st.name = "lost.opus";
        w.feed(0, st);
        Input d(Input::STREAM_DATA);
        d.chat = "g";
        d.name = "lost.opus";
        d.bytes = bytes_of(6000, 2);
        w.feed(0, d);
        for (int k = 0; k < 4; k++) w.act();
        auto& alice = w.members[0];
        w.queue.erase(std::remove_if(w.queue.begin(), w.queue.end(), [](auto& q) { return q.first == 0; }), w.queue.end());
        w.later.erase(std::remove_if(w.later.begin(), w.later.end(), [](auto& l) { return l.who == 0; }), w.later.end());
        alice.eng = Engine(kq::Store(kq::memory_files(*alice.files)).load(alice.name), 1530);
        alice.eng.radio.insert("g");
        w.release(0, alice.eng.resume(0));
        w.run_to(w.clock + 600000);
        bool lost = noticed(w, 0, "a message could not be sent: what it held was lost") && w.chat(0, "g")->jobs.empty();
        check(lost && exact(bytes_of(3000, 3), "after.opus"), v, "a restart while it is made loses it; the next one goes");
    }
    {
        // Our own burst in a radio group is held in memory only: no file, no line, no spool, and
        // not in a saved job even when it would fit one box; a restart loses it.
        World w;
        found_and_join(w);
        for (auto& m : w.members) m.eng.radio.insert("g");
        std::ifstream in("vectors/hushphone/tone.opus", std::ios::binary);
        Bytes tone((std::istreambuf_iterator<char>(in)), {});
        auto o = kq::ogg::demux(tone.data(), tone.size());
        auto record = [&](size_t n) {
            kq::radio::Recorder rec(9);
            for (size_t i = 0; i < n; i++) {
                Bytes m{kq::radio::kEvBatch, 1, (uint8_t)(o->packets[i].size() + 1), 0, kq::radio::kCodecOpus};
                m.insert(m.end(), o->packets[i].begin(), o->packets[i].end());
                rec.batch(m.data(), m.size());
            }
            rec.end();
            uint8_t r32[32] = {(uint8_t)n};
            return std::make_pair(rec.take(), kq::radio::burst_name(r32));
        };
        auto [clip, name] = record(5);
        check(clip.size() + 200 < 1530, v, "a short burst, that would fit one box");
        send_file(w, 0, name, clip);
        auto unsaved = [&](size_t i, const Bytes& b) {
            std::string raw(b.end() - 40, b.end()), hex = tohex(Bytes(b.end() - 40, b.end()));
            bool found = false;
            for (auto& [_, d] : *w.members[i].files) found |= d.find(raw) != std::string::npos || d.find(hex) != std::string::npos;
            for (auto& [_, d] : *w.members[i].kept) found |= std::string(d.begin(), d.end()).find(raw) != std::string::npos;
            for (auto& l : w.chat(i, "g")->log) found |= l.text.find(".opus") != std::string::npos;
            return !found;
        };
        check(unsaved(0, clip), v, "ours is not saved while it is written");
        w.run_to(w.clock + 600000);
        auto& bob = w.members[1];
        bool ibox = false;
        for (auto& [k, b] : w.sim.boxes) ibox |= !b.second.empty() && b.second[0] == 'I';
        check(bob.clips.size() == 1 && bob.clips[0].bytes == clip && ibox && unsaved(0, clip) && unsaved(1, clip), v,
              "it is heard, through a substream, and kept by neither");
        auto [clip2, name2] = record(50);
        send_file(w, 0, name2, clip2);
        for (int k = 0; k < 3; k++) w.act();
        auto& alice = w.members[0];
        w.queue.erase(std::remove_if(w.queue.begin(), w.queue.end(), [](auto& q) { return q.first == 0; }), w.queue.end());
        w.later.erase(std::remove_if(w.later.begin(), w.later.end(), [](auto& l) { return l.who == 0; }), w.later.end());
        alice.eng = Engine(kq::Store(kq::memory_files(*alice.files)).load(alice.name), 1530);
        alice.eng.radio.insert("g");
        w.release(0, alice.eng.resume(0));
        w.run_to(w.clock + 600000);
        check(noticed(w, 0, "a message could not be sent: what it held was lost") && w.chat(0, "g")->jobs.empty() &&
                  unsaved(0, clip2), v, "a restart while it is written loses it, and says so");
    }
    {
        World w;
        found_and_join(w);
        Bytes doc = bytes_of(30000, 5);
        send_file(w, 0, "big.bin", doc);
        auto out = [&] {
            size_t n = 0;
            for (auto& q : w.queue) n += q.second.kind == Action::WRITE_CHUNK;
            return n;
        };
        for (int k = 0; k < 1000 && out() < WINDOW; k++) if (!w.act()) w.run_to(w.clock + 1000);
        check(out() == WINDOW, v, "a window of chunks written at once");
        std::reverse(w.queue.begin(), w.queue.end());
        w.run_to(w.clock + 600000);
        check(file(w, 1, "big.bin") == doc, v, "chunks landing out of order");
    }
    {
        // A voice burst in a radio group is handed out to play, and nothing of it is kept: no
        // file, no line, nothing in the saved state. Other files there are kept as anywhere.
        World w;
        found_and_join(w);
        w.members[1].eng.radio.insert("g");
        std::ifstream in("vectors/hushphone/tone.opus", std::ios::binary);
        Bytes clip((std::istreambuf_iterator<char>(in)), {});
        send_file(w, 0, "21eae8f1c3d04dd5.opus", clip);
        send_file(w, 0, "a.png", bytes_of(500, 7));
        w.run_to(w.clock + 600000);
        auto& bob = w.members[1];
        check(clip.size() == 1668 && bob.clips.size() == 1 && bob.clips[0].bytes == clip && bob.clips[0].chat == "g" &&
                  bob.clips[0].sender == w.chat(0, "g")->group->me(), v, "a radio burst is handed out whole");
        bool line = false;
        for (auto& l : w.chat(1, "g")->log) line |= l.text.find(".opus") != std::string::npos;
        std::string raw(clip.begin() + 200, clip.begin() + 232), hex = tohex(Bytes(clip.begin() + 200, clip.begin() + 232));
        bool stored = false;
        for (auto& [_, d] : *bob.files) stored |= d.find(raw) != std::string::npos || d.find(hex) != std::string::npos;
        for (auto& [_, d] : *bob.kept) stored |= d == clip || std::string(d.begin(), d.end()).find(raw) != std::string::npos;
        check(!line && !stored, v, "and is not kept: no line, no file, nothing in the saved state");
        check(file(w, 1, "a.png") == bytes_of(500, 7) && file(w, 0, "21eae8f1c3d04dd5.opus") == clip, v,
              "an image in a radio group is kept, and so is the sender's own burst");
    }
    {
        World w;
        found_and_join(w);
        Bytes doc = bytes_of(30000, 9);
        send_file(w, 0, "restart.bin", doc);
        for (int k = 0; k < 6; k++) w.act();
        // A restart: what was under way is lost, the state is what was saved.
        auto& m = w.members[0];
        w.queue.erase(std::remove_if(w.queue.begin(), w.queue.end(), [](auto& q) { return q.first == 0; }), w.queue.end());
        w.later.erase(std::remove_if(w.later.begin(), w.later.end(), [](auto& l) { return l.who == 0; }), w.later.end());
        m.eng = Engine(kq::Store(kq::memory_files(*m.files)).load(m.name), 1530);
        w.release(0, m.eng.resume(0));
        w.run_to(w.clock + 600000);
        check(file(w, 1, "restart.bin") == doc && spooled(w, 0) == 0, v, "an upload goes on after a restart");
    }
    {
        World w;
        found_and_join(w);
        send_file(w, 0, "huge.bin", Bytes(MAX_FILE + 1));
        check(noticed(w, 0, "that file is too large to send: at most 16.0 MB"), v, "a file too large to send");
        w.members[1].eng.max_file = 10000;
        send_file(w, 0, "big.bin", bytes_of(30000, 1));
        send_file(w, 0, "a.png", bytes_of(500, 7));
        w.run_to(w.clock + 600000);
        check(noticed(w, 1, "sent a file too large to fetch") && !file(w, 1, "big.bin"), v,
              "a file past the reader's limit is not fetched");
        check(file(w, 1, "a.png") == bytes_of(500, 7), v, "and what follows it is read");
        std::string a = attachment(w, 1, "a.png")->file;
        Input rm(Input::REMOVE_MEMBER);
        rm.chat = "g";
        rm.who = w.chat(0, "g")->group->me();
        w.feed(1, rm);
        w.run_to(w.clock + 600000);
        check(!w.members[1].kept->count(a), v, "a removed member's files are deleted");
        w.command({"leave", "alice", "g"});
        w.run_to(w.clock + 600000);
        check(w.members[0].kept->empty(), v, "leaving deletes every file");
    }
}

// A ping is content: its readers get a notice, radio listeners hear it, nobody gets a line.
static void ping_tests() {
    Vec v{"client", "ping", {}};
    World w;
    found_and_join(w);
    auto run = [&] { w.run_to(w.clock + 600000); };
    auto ping = [&](size_t i) {
        Input p(Input::PING);
        p.chat = "g";
        w.feed(i, p);
        run();
    };
    auto lines = [&](size_t i) {
        size_t n = 0;
        for (auto& l : w.chat(i, "g")->log) n += !l.notice;
        return n;
    };
    size_t a0 = lines(0), b0 = lines(1);
    ping(0);
    check(w.members[1].pings.empty() && noticed(w, 1, "~alice pinged") && !noticed(w, 0, "pinged"), v,
          "not on the radio: a notice, and no sound");
    for (auto& m : w.members) m.eng.radio.insert("g");
    ping(0);
    ping(0);
    auto& p = w.members[1].pings;
    check(p.size() == 2 && p[0].chat == "g" && p[0].who == "~alice" && p[0].sender == w.chat(0, "g")->group->me() &&
              w.members[0].pings.empty(), v, "on the radio: each heard, from ~alice, not by its sender");
    Input x(Input::SEND);
    x.chat = "g";
    x.text = "after the pings";
    w.feed(0, x);
    run();
    check(says(w, 1, "after the pings") && lines(1) == b0 + 1 && lines(0) == a0 + 1, v,
          "a message after them arrives, and they made no line");
    ping(1);
    const Line* sent = nullptr;
    for (auto& l : w.chat(0, "g")->log) if (l.text == "after the pings") sent = &l;
    check(w.members[0].pings.size() == 1 && w.members[0].pings[0].who == "~bob", v, "and back the other way");
    check(sent && w.chat(0, "g")->acknowledged(*sent), v, "bob's ping carries his acks");

    // A type this client does not know, written raw: a notice naming it, and what follows is read.
    Chat* c = w.members[0].eng.state.chat("g");
    Gcm g;
    g.version = CURRENT_VERSION;
    g.membership_hash = c->group->membership(nullptr);
    g.msg_type = 12;
    Bytes cbor = g.to_bytes();
    for (auto& o : c->group->compose(cbor)) {
        Job j;
        j.id = c->fresh();
        j.token = o.token;
        j.box = frame(cbor);
        c->jobs.push_back(j);
    }
    x.text = "after the unknown";
    w.feed(0, x);
    run();
    check(says(w, 1, "after the unknown") &&
              noticed(w, 1, "~alice sent a message this app cannot show (type 12, version 4): update the app"), v,
          "an unknown type is noticed, and what follows it read");
}

int main() {
    if (sodium_init() < 0) return 2;
    const char* env = getenv("KQ1_TESTDATA");
    fs::path dir = fs::path(env ? env : "../../../../pigeonhole_protocols/testdata/katzenqt1") / "client_scenarios";
    if (!fs::is_directory(dir)) { fprintf(stderr, "no directory %s (set KQ1_TESTDATA)\n", dir.c_str()); return 2; }
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dir)) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    Vec v{"client", "", {}};
    check(files.size() == 8, v, "8 client scenarios");
    for (auto& f : files) {
        Vec s{"client", f.filename().string(), {}};
        std::ifstream in(f);
        std::string script((std::istreambuf_iterator<char>(in)), {});
        try {
            std::string got = run_script(script);
            if (got != script) {
                auto split = [](const std::string& t) {
                    std::vector<std::string> v;
                    std::istringstream is(t);
                    for (std::string l; std::getline(is, l);) v.push_back(l);
                    return v;
                };
                auto a = split(got), b = split(script);
                for (size_t n = 0; n < std::max(a.size(), b.size()); n++) {
                    std::string la = n < a.size() ? a[n] : "<end>", lb = n < b.size() ? b[n] : "<end>";
                    if (la != lb) { fprintf(stderr, "%s line %zu:\n  got      %s\n  expected %s\n", f.c_str(), n + 1, la.c_str(), lb.c_str()); break; }
                }
            }
            check(got == script, s, "runs as written");
        } catch (const std::exception& e) {
            fprintf(stderr, "%s: %s\n", f.c_str(), e.what());
            check(false, s, "runs");
        }
    }
    file_tests(getenv("KQ_FILES_OUT"));
    ping_tests();
    if (const char* out = getenv("KQ_FILES_OUT")) {
        FILE* f = fopen(out, "w");
        if (f) {
            fprintf(f, "%s\n", Json::obj({{"vectors", Json::arr(g_dump)}}).dump().c_str());
            fclose(f);
        }
    }
    check(g_unsaved == 0 && g_saved > 0, v, "every step's state is saved");
    printf("client: %d steps saved and loaded back\n", g_saved);
    return finish("client");
}
