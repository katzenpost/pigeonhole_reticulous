// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_runtime.h"

#include <sodium.h>

#include "kq1_voucher.h"

namespace kq {

// ---- Runtime ----

void Runtime::release(kq1::StepResult&& r) {
    steps++;
    if (on_file) for (auto& f : r.files) on_file(f);
    if (on_clip) for (auto& c : r.clips) on_clip(std::move(c));
    if (on_ping) for (auto& p : r.pings) on_ping(p);
    for (auto& a : r.actions) ready_.push_back(a);
    if (on_changes && !r.changes.empty()) on_changes(r.changes);
}

void Runtime::input(int64_t now_s, uint64_t, const kq1::Input& in) { release(eng.step(now_s, in)); }

void Runtime::resume(int64_t now_s) { release(eng.resume(now_s)); }

std::optional<kq1::Action> Runtime::take() {
    if (ready_.empty()) return std::nullopt;
    kq1::Action a = std::move(ready_.front());
    ready_.pop_front();
    return a;
}

std::optional<kq1::Action> Runtime::take_if(const std::function<bool(const kq1::Action&)>& ok) {
    for (auto it = ready_.begin(); it != ready_.end(); ++it) {
        if (!ok(*it)) continue;
        kq1::Action a = std::move(*it);
        ready_.erase(it);
        return a;
    }
    return std::nullopt;
}

void Runtime::finished(int64_t now_s, uint64_t now_ms, const kq1::Done& d) {
    switch (d.kind) {
        case kq1::Done::DONE: if (d.input) input(now_s, now_ms, *d.input); break;
        case kq1::Done::RETRY: later_.push_back({now_ms + d.ms, d.action, std::nullopt}); break;
        case kq1::Done::LATER: later_.push_back({now_ms + d.ms, std::nullopt, d.input}); break;
        case kq1::Done::NOTHING: break;
    }
}

void Runtime::tick(int64_t now_s, uint64_t now_ms) {
    std::vector<Later> due, rest;
    for (auto& l : later_) (l.due <= now_ms ? due : rest).push_back(std::move(l));
    later_ = std::move(rest);
    for (auto& l : due) {
        if (l.act) { if (eng.state.wanted(*l.act)) ready_.push_back(*l.act); }
        else input(now_s, now_ms, *l.in);
    }
}

std::optional<uint64_t> Runtime::next_due() const {
    std::optional<uint64_t> d;
    for (auto& l : later_) if (!d || l.due < *d) d = l.due;
    return d;
}

// ---- ReplicaBackend ----

static void random_rng(uint8_t* p, size_t n) { randombytes_buf(p, n); }

std::optional<kq1::Keypair> ReplicaBackend::new_keypair() {
    auto w = bacap::WriteCap::generate(random_rng);
    return kq1::Keypair{w.bytes(), w.read_cap().bytes(), w.index.bytes()};
}

kq1::Backend::Write ReplicaBackend::write_box(const Bytes& write_cap, const kq1::Index& index, const Bytes& plaintext) {
    auto w = bacap::WriteCap::from(write_cap.data(), write_cap.size());
    auto i = bacap::Index::from(index.data(), index.size());
    if (!w || !i) { error_ = "a malformed capability"; return Write::FAILED; }
    uint8_t code = wire::INTERNAL_ERROR;
    bool queued = true;
    bridge_([&](std::function<void()> done) {
        queued = client_.write(*w, *i, plaintext, [&code, done](uint8_t c) { code = c; done(); });
        if (!queued) done();
    });
    if (!queued) { error_ = "a message too long for a box"; return Write::FAILED; }
    if (code == wire::SUCCESS) return Write::OK;
    if (code == wire::ALREADY_EXISTS) return Write::EXISTS;
    error_ = "the replica answered " + std::to_string(code);
    return Write::FAILED;
}

std::optional<kq1::Read> ReplicaBackend::read_box(const Bytes& read_cap, const kq1::Index& index) {
    auto r = bacap::ReadCap::from(read_cap.data(), read_cap.size());
    auto i = bacap::Index::from(index.data(), index.size());
    if (!r || !i) { error_ = "a malformed capability"; return std::nullopt; }
    ReadOutcome o;
    bridge_([&](std::function<void()> done) {
        client_.read(r->root_pk, *i, [&o, done](const ReadOutcome& x) { o = x; done(); });
    });
    kq1::Read out;
    switch (o.kind) {
        case ReadOutcome::FOUND: out.kind = kq1::Read::DATA; out.data = std::move(o.plaintext); return out;
        case ReadOutcome::NOT_FOUND: out.kind = kq1::Read::NOT_FOUND; return out;
        case ReadOutcome::TOMBSTONE: out.kind = kq1::Read::TOMBSTONE; return out;
        default: error_ = "no answer from the replica"; return std::nullopt;
    }
}

std::optional<kq1::Minted> ReplicaBackend::voucher_mint(const Bytes& write_cap, const std::string& name, uint8_t form) {
    auto m = kq1::voucher::mint(write_cap, name, form == 1 ? kq1::voucher::LONG : kq1::voucher::COMPACT, random_rng);
    if (!m) { error_ = "could not mint a voucher"; return std::nullopt; }
    seal_pub_[m->secret_key] = m->public_key;
    kq1::Minted out;
    out.voucher.assign(m->voucher.begin(), m->voucher.end());
    out.payload = m->payload;
    out.write_cap = m->stream.bytes();
    out.read_cap = m->stream.read_cap().bytes();
    out.secret_key = m->secret_key;
    out.version = (uint8_t)m->form;
    return out;
}

std::optional<std::pair<Bytes, Bytes>> ReplicaBackend::voucher_derive_stream(const Bytes& voucher) {
    if (voucher.size() != 32) { error_ = "not a voucher"; return std::nullopt; }
    auto w = kq1::voucher::derive_stream(voucher.data());
    return std::make_pair(w.bytes(), w.read_cap().bytes());
}

std::optional<kq1::Inducted> ReplicaBackend::voucher_induct(const Bytes& voucher, const Bytes& payload,
                                                            const Bytes& who_reply) {
    if (voucher.size() != 32) { error_ = "not a voucher"; return std::nullopt; }
    Bytes salt(kq1::voucher::kSaltSize);
    randombytes_buf(salt.data(), salt.size());
    auto r = kq1::voucher::induct(voucher.data(), payload, who_reply, salt, random_rng);
    if (!r) { error_ = "the voucher's payload does not check out"; return std::nullopt; }
    return kq1::Inducted{r->name, r->mutated_read_cap, r->sealed, (uint8_t)r->form};
}

std::optional<kq1::Opened> ReplicaBackend::voucher_open(const Bytes& secret_key, const Bytes& sealed,
                                                        const Bytes& write_cap) {
    auto it = seal_pub_.find(secret_key);
    Bytes pub = it != seal_pub_.end() ? it->second : Bytes();
    auto o = kq1::voucher::open_reply(secret_key, pub, sealed, write_cap);
    if (!o) { error_ = "the reply would not open"; return std::nullopt; }
    return kq1::Opened{o->who_reply, o->mutated_write_cap};
}

// ---- what the screens and the CLI show, and the commands they send ----

std::vector<KqChat> chats(const kq1::State& st, const std::string& only, bool lines) {
    std::vector<KqChat> out;
    for (auto& c : st.chats) {
        if (!only.empty() && c.id != only) continue;
        KqChat k;
        k.id = c.id;
        k.joined = c.group.has_value();
        k.adding = c.inducting.has_value();
        if (c.joining) k.voucher = c.joining->code.empty() ? "\xE2\x80\xA6" : c.joining->code;
        k.members = c.group ? (unsigned)c.group->readers.size() : 0;
        k.pending = (unsigned)c.jobs.size();
        auto line = [&](const kq1::Line& l) {
            KqLine x;
            x.time = l.time;
            if (l.notice) {
                x.notice = true;
                // The shared wording names the file katzenrns keeps the code in; there is none here.
                x.text = l.notice->kind == kq1::NoticeKind::VoucherMinted
                             ? "give the voucher code to a member of the group"
                             : kq1::plain(*l.notice);
            } else {
                x.mine = c.group && c.group->me() == l.sender;
                const std::string* n = c.names.get(l.sender);
                x.who = x.mine ? "me" : n ? *n : "?";
                x.text = l.text;
                if (x.mine)
                    x.mark = l.status == kq1::Status::Pending ? KqLine::PENDING
                           : c.acknowledged(l) ? KqLine::READ : KqLine::WRITTEN;
            }
            return x;
        };
        for (auto it = c.log.rbegin(); it != c.log.rend(); ++it)
            if (!it->notice) { k.last = line(*it); break; }
        if (lines)
            for (auto& l : c.log) k.lines.push_back(line(l));
        out.push_back(std::move(k));
    }
    return out;
}

std::optional<kq1::Input> input_of(const std::string& verb, const std::string& group, const std::string& arg) {
    static const std::map<std::string, kq1::Input::Kind> kinds = {
        {"create", kq1::Input::CREATE}, {"join", kq1::Input::JOIN},   {"invite", kq1::Input::INDUCT},
        {"send", kq1::Input::SEND},     {"scan", kq1::Input::SCAN},   {"leave", kq1::Input::LEAVE},
        {"ping", kq1::Input::PING}};
    auto k = kinds.find(verb);
    if (k == kinds.end()) return std::nullopt;
    kq1::Input in(k->second);
    in.chat = group;
    in.name = group;
    in.code = arg;
    in.text = arg;
    return in;
}

}  // namespace kq
