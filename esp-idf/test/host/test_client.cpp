// The katzenqt1 engine and executor against group_chat_client's client scenarios, through an
// in-memory transport, as pigeonhole_protocols rust/tests/katzenqt1_client_scenarios.rs and its
// harness (tests/katzenqt1_client_harness) with transport/sim.rs. The scenarios are read from
// KQ1_TESTDATA, a pigeonhole_protocols checkout's testdata/katzenqt1.
#include <sodium.h>

#include <cstring>
#include <filesystem>
#include <sstream>

#include "kq1_engine.h"
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
                                  "ReadVoucher", "Seal", "Publish"};
    return names[a.kind];
}

struct Client {
    std::string name;
    Engine eng;
};

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

    size_t who(const std::string& name) {
        for (size_t i = 0; i < members.size(); i++) if (members[i].name == name) return i;
        members.push_back({name, Engine(State{name, {}}, 1530)});
        return members.size() - 1;
    }
    void feed(size_t i, const Input& input) { release(i, members[i].eng.step(0, input)); }
    void release(size_t i, const StepResult& res) {
        for (auto& a : res.actions) queue.push_back({i, a});
    }
    bool act() {
        auto it = std::find_if(queue.begin(), queue.end(), [&](auto& q) { return !held.count(q.first); });
        if (it == queue.end()) return false;
        auto [w, a] = *it;
        queue.erase(it);
        Executor x{sim, [](uint64_t) {}};
        std::string kind = kind_of(a);
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
    return finish("client");
}
