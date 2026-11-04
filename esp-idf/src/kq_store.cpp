// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_store.h"

#include <fcntl.h>
#include <sodium.h>
#include <unistd.h>

#include <cstdio>

#include "kq1_trace.h"

namespace kq {

using namespace kq1;
using namespace kq1::trace;

namespace {

// A conversation's id can be any text; its files are named by its hash.
std::string base(const ChatId& id) {
    uint8_t d[16];
    crypto_generichash_blake2b(d, sizeof d, (const uint8_t*)id.data(), id.size(), nullptr, 0);
    static const char* x = "0123456789abcdef";
    std::string s = "c";
    for (uint8_t c : d) { s += x[c >> 4]; s += x[c & 15]; }
    return s;
}

Json text(const std::string& s) { return Json::str(s); }

Json chat_json(const Chat& c) {
    std::vector<Json> cursors, names, jobs;
    for (auto& [k, v] : c.cursors) cursors.push_back(Json::arr({h(k), h(v)}));
    for (auto& [k, v] : c.names) names.push_back(Json::arr({h(k), text(v)}));
    for (auto& j : c.jobs)
        jobs.push_back(Json::obj({{"id", nu(j.id)}, {"token", on(j.token)}, {"index", oh(j.index)}, {"box", h(j.box)},
                                  {"line", on(j.line)}}));
    Json joining, inducting;
    if (auto& j = c.joining)
        joining = Json::obj({{"code", text(j->code)}, {"secret_key", h(j->secret_key)},
                             {"voucher_read_cap", h(j->voucher_read_cap)}, {"box1", h(j->box1)}, {"version", nu(j->version)}});
    if (auto& i = c.inducting) {
        Json sealed;
        if (auto& s = i->sealed)
            sealed = Json::obj({{"name", text(s->name)}, {"mutated_read_cap", h(s->mutated_read_cap)}, {"reply", h(s->reply)},
                                {"version", nu(s->version)}});
        inducting = Json::obj({{"voucher", h(i->voucher)}, {"payload", oh(i->payload)},
                               {"voucher_write_cap", h(i->voucher_write_cap)}, {"box1", h(i->box1)},
                               {"sealing", Json::boolean(i->sealing)}, {"sealed", sealed}});
    }
    return Json::obj({{"id", text(c.id)}, {"name", text(c.name)}, {"write_cap", h(c.write_cap)},
                      {"next_write", h(c.next_write)}, {"group", c.group ? group_j(*c.group) : Json{}},
                      {"cursors", Json::arr(cursors)}, {"names", Json::arr(names)}, {"jobs", Json::arr(jobs)},
                      {"joining", joining}, {"inducting", inducting}, {"next_id", nu(c.next_id)}});
}

std::string str(const Json& j) {
    if (j.kind != Json::STR) throw Bad("not a string: " + j.dump());
    return j.s;
}

Chat chat_of(const Json& j) {
    Chat c;
    c.id = str(get(j, "id"));
    c.name = str(get(j, "name"));
    c.write_cap = b(get(j, "write_cap"));
    c.next_write = b(get(j, "next_write"));
    c.next_id = u(get(j, "next_id"));
    if (!get(j, "group").is_null()) c.group = read_group(j["group"]);
    each_pair(get(j, "cursors"), [&](const Json& k, const Json& v) { c.cursors.insert(b(k), b(v)); });
    each_pair(get(j, "names"), [&](const Json& k, const Json& v) { c.names.insert(b(k), str(v)); });
    for (auto& x : arr(get(j, "jobs")))
        c.jobs.push_back(Job{(size_t)u(get(x, "id")), ous(get(x, "token")), ob(get(x, "index")), b(get(x, "box")),
                             ous(get(x, "line"))});
    if (auto& jn = get(j, "joining"); !jn.is_null())
        c.joining = Joining{str(get(jn, "code")), b(get(jn, "secret_key")), b(get(jn, "voucher_read_cap")),
                            b(get(jn, "box1")), (uint8_t)u(get(jn, "version"))};
    if (auto& i = get(j, "inducting"); !i.is_null()) {
        Inducting in{b(get(i, "voucher")), ob(get(i, "payload")), b(get(i, "voucher_write_cap")), b(get(i, "box1")),
                     bo(get(i, "sealing")), std::nullopt};
        if (auto& s = get(i, "sealed"); !s.is_null())
            in.sealed = Sealed{str(get(s, "name")), b(get(s, "mutated_read_cap")), b(get(s, "reply")),
                               (uint8_t)u(get(s, "version"))};
        c.inducting = in;
    }
    return c;
}

Json line_json(const Line& l) {
    Json notice;
    if (l.notice) {
        std::vector<Json> args;
        for (auto& a : l.notice->args) args.push_back(text(a));
        notice = Json::obj({{"kind", nu((unsigned)l.notice->kind)}, {"args", Json::arr(args)}});
    }
    return Json::obj({{"id", nu(l.id)}, {"sender", h(l.sender)}, {"index", h(l.index)}, {"text", text(l.text)},
                      {"notice", notice}, {"status", Json::boolean(l.status == Status::Pending)},
                      {"idx64", l.idx64 ? nu(*l.idx64) : Json{}}, {"time", Json::neg(l.time)}});
}

Line line_of(const Json& j) {
    Line l;
    l.id = u(get(j, "id"));
    l.sender = b(get(j, "sender"));
    l.index = b(get(j, "index"));
    l.text = str(get(j, "text"));
    if (auto& n = get(j, "notice"); !n.is_null()) {
        uint64_t k = u(get(n, "kind"));
        if (k > (uint64_t)NoticeKind::Desync) throw Bad("unknown notice");
        Notice no{(NoticeKind)k, {}};
        for (auto& a : arr(get(n, "args"))) no.args.push_back(str(a));
        l.notice = no;
    }
    l.status = bo(get(j, "status")) ? Status::Pending : Status::Sent;
    if (!get(j, "idx64").is_null()) l.idx64 = u(j["idx64"]);
    auto& t = get(j, "time");
    if (t.kind != Json::NUM) throw Bad("time");
    l.time = std::stoll(t.s);
    return l;
}

Json drop_json(size_t id) { return Json::obj({{"drop", nu(id)}}); }

}  // namespace

bool Store::put(const std::string& name, const std::string& data, bool whole) {
    writes++;
    bytes += data.size();
    return whole ? f_.replace(name, data) : f_.append(name, data);
}

bool Store::save_log(const Chat& c) {
    std::string all;
    for (auto& l : c.log) all += line_json(l).dump() + "\n";
    entries_[c.id] = c.log.size();
    return put(base(c.id) + ".log", all, true);
}

kq1::State Store::load(const std::string& me, std::vector<std::string>* problems) {
    State st{me, {}};
    auto note = [&](const std::string& p) { if (problems) problems->push_back(p); };
    auto meta = f_.read("state.json");
    if (!meta) return st;
    std::vector<std::string> order;
    try {
        Json m = Json::parse(*meta);
        st.me = str(get(m, "me"));
        for (auto& id : arr(get(m, "order"))) order.push_back(str(id));
    } catch (const std::exception& e) {
        note(std::string("state.json: ") + e.what());
        return st;
    }
    meta_ = *meta;
    for (auto& id : order) {
        std::string b = base(id);
        auto rec = f_.read(b + ".json");
        if (!rec) { note(id + ": no record"); continue; }
        Chat c;
        try {
            c = chat_of(Json::parse(*rec));
        } catch (const std::exception& e) {
            note(id + ": " + e.what());
            continue;
        }
        std::map<size_t, Line> lines;
        size_t n = 0;
        auto log = f_.read(b + ".log").value_or("");
        for (size_t p = 0; p < log.size();) {
            size_t q = log.find('\n', p);
            if (q == std::string::npos) break;  // cut short as it was written: the step it was in is redone
            try {
                Json j = Json::parse(log.substr(p, q - p));
                if (j.o.count("drop")) lines.erase(u(j["drop"]));
                else { Line l = line_of(j); lines[l.id] = l; }
                n++;
            } catch (const std::exception& e) {
                note(id + ": a line: " + e.what());
            }
            p = q + 1;
        }
        for (auto& [_, l] : lines) c.log.push_back(std::move(l));
        saved_[id] = *rec;
        entries_[id] = n;
        st.chats.push_back(std::move(c));
    }
    return st;
}

bool Store::commit(const kq1::State& st, const kq1::Changes& ch) {
    bool ok = true;
    for (auto& id : ch.removed) {
        f_.remove(base(id) + ".json");
        f_.remove(base(id) + ".log");
        saved_.erase(id);
        entries_.erase(id);
    }
    std::set<ChatId> logged;
    auto journal = [&](const ChatId& id, const std::string& more, size_t n) {
        const Chat* c = st.chat(id);
        if (!c) return;
        size_t& e = entries_[id];
        // A journal grown to twice its lines is written again whole.
        if (e + n > 2 * c->log.size() + 64) { if (!logged.count(id)) ok &= save_log(*c); logged.insert(id); return; }
        if (logged.count(id)) return;
        e += n;
        ok &= put(base(id) + ".log", more, false);
    };
    for (auto& id : ch.chats) {
        const Chat* c = st.chat(id);
        if (!c) continue;
        std::string rec = chat_json(*c).dump();
        if (saved_[id] == rec) continue;
        if (put(base(id) + ".json", rec, true)) saved_[id] = rec;
        else ok = false;
    }
    for (auto& [id, ids] : ch.lines) {
        const Chat* c = st.chat(id);
        if (!c) continue;
        std::string more;
        size_t n = 0;
        for (auto& l : c->log)
            if (ids.count(l.id)) { more += line_json(l).dump() + "\n"; n++; }
        if (n) journal(id, more, n);
    }
    for (auto& [id, ids] : ch.dropped) {
        std::string more;
        for (size_t i : ids) more += drop_json(i).dump() + "\n";
        if (!ids.empty()) journal(id, more, ids.size());
    }
    std::vector<Json> order;
    for (auto& c : st.chats) order.push_back(Json::str(c.id));
    std::string meta = Json::obj({{"me", Json::str(st.me)}, {"order", Json::arr(order)}}).dump();
    if (meta != meta_) {
        if (put("state.json", meta, true)) meta_ = meta;
        else ok = false;
    }
    return ok;
}

StoreFiles memory_files(std::map<std::string, std::string>& files) {
    StoreFiles f;
    f.read = [&files](const std::string& n) -> std::optional<std::string> {
        auto it = files.find(n);
        if (it == files.end()) return std::nullopt;
        return it->second;
    };
    f.replace = [&files](const std::string& n, const std::string& d) { files[n] = d; return true; };
    f.append = [&files](const std::string& n, const std::string& d) { files[n] += d; return true; };
    f.remove = [&files](const std::string& n) { files.erase(n); };
    return f;
}

std::string state_dump(const kq1::State& st) {
    std::string s = st.me + "\n";
    for (auto& c : st.chats) {
        s += chat_json(c).dump() + "\n";
        for (auto& l : c.log) s += line_json(l).dump() + "\n";
    }
    return s;
}

StoreFiles posix_files(const std::string& dir) {
    auto path = [dir](const std::string& n) { return dir + "/" + n; };
    auto write = [](const std::string& p, const std::string& d, int flags) {
        int fd = open(p.c_str(), O_WRONLY | O_CREAT | flags, 0600);
        if (fd < 0) return false;
        bool ok = ::write(fd, d.data(), d.size()) == (ssize_t)d.size() && fsync(fd) == 0;
        return close(fd) == 0 && ok;
    };
    StoreFiles f;
    f.read = [path](const std::string& n) -> std::optional<std::string> {
        FILE* fp = fopen(path(n).c_str(), "rb");
        if (!fp) return std::nullopt;
        std::string s;
        char buf[4096];
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, fp)) > 0) s.append(buf, k);
        fclose(fp);
        return s;
    };
    f.replace = [path, write](const std::string& n, const std::string& d) {
        std::string tmp = path(n) + ".new";
        return write(tmp, d, O_TRUNC) && rename(tmp.c_str(), path(n).c_str()) == 0;
    };
    f.append = [path, write](const std::string& n, const std::string& d) { return write(path(n), d, O_APPEND); };
    f.remove = [path](const std::string& n) { unlink(path(n).c_str()); };
    return f;
}

}  // namespace kq
