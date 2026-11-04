// SPDX-License-Identifier: AGPL-3.0-only
// The store on its own: text of any kind comes back as written, a journal cut short loses only
// its last line, a long journal is written again whole, and the POSIX files hold it all.
#include <sodium.h>

#include <algorithm>
#include <filesystem>

#include "kq_store.h"
#include "vectors.h"

using namespace kq1;

static Chat chat(const std::string& id, size_t lines) {
    Chat c;
    c.id = c.name = id;
    c.write_cap = Bytes(32, 7);
    c.next_write = Bytes(104, 9);
    c.names.insert(Bytes(32, 1), "kate \"k\" \\ \xC3\xA9");
    for (size_t i = 0; i < lines; i++) {
        Line l;
        l.id = c.fresh();
        l.sender = Bytes(32, 1);
        l.index = Bytes(104, (uint8_t)i);
        l.text = "line " + std::to_string(i) + ": \"quoted\"\n\ttab \x01 \xE2\x9C\x93";
        l.time = 1700000000 + (int64_t)i;
        l.idx64 = i;
        c.log.push_back(l);
    }
    Line n;
    n.id = c.fresh();
    n.notice = Notice{NoticeKind::Added, {"~kate", "x"}};
    n.time = -5;
    c.log.push_back(n);
    return c;
}

static Changes all(const State& st) {
    Changes ch;
    for (auto& c : st.chats) {
        ch.chats.insert(c.id);
        for (auto& l : c.log) ch.lines[c.id].insert(l.id);
    }
    return ch;
}

int main() {
    if (sodium_init() < 0) return 2;
    Vec v{"store", "", {}};
    std::map<std::string, std::string> files;
    State st{"tdeck", {chat("a group", 3), chat("b", 0)}};
    kq::Store s(kq::memory_files(files));
    check(s.commit(st, all(st)), v, "commit");
    kq::Store r(kq::memory_files(files));
    check(kq::state_dump(r.load("other")) == kq::state_dump(st), v, "loads as saved, with its own name");
    check(kq::Store(kq::memory_files(files)).load("x").me == "tdeck", v, "me is saved");

    // A status change appends to the journal; the record is not written again.
    size_t w = s.writes;
    st.chats[0].log[1].status = Status::Pending;
    Changes one;
    one.chats.insert("a group");
    one.lines["a group"].insert(1);
    check(s.commit(st, one), v, "commit a line");
    check(s.writes == w + 1, v, "one append, no record");
    check(kq::state_dump(kq::Store(kq::memory_files(files)).load("x")) == kq::state_dump(st), v, "the line's change loads");

    // A journal cut short in its last line: that line is lost, the rest loads.
    std::string log;
    for (auto& [n, d] : files)
        if (n.size() > 4 && n.substr(n.size() - 4) == ".log" && d.size() > log.size()) log = n;
    std::string whole = files[log];
    files[log] = whole + "{\"id\":9,\"send";
    std::vector<std::string> problems;
    State cut = kq::Store(kq::memory_files(files)).load("x", &problems);
    check(kq::state_dump(cut) == kq::state_dump(st) && problems.empty(), v, "a torn last line is ignored");
    files[log] = whole;

    // Dropped lines, and a journal that has grown is written again whole.
    for (int k = 0; k < 100; k++) {
        st.chats[0].log[0].text = "edit " + std::to_string(k);
        Changes c1;
        c1.lines["a group"].insert(0);
        s.commit(st, c1);
    }
    size_t entries = (size_t)std::count(files[log].begin(), files[log].end(), '\n');
    check(entries <= 2 * st.chats[0].log.size() + 64, v, "the journal is compacted");
    st.chats[0].log.erase(st.chats[0].log.begin());
    Changes d;
    d.dropped["a group"].insert(0);
    s.commit(st, d);
    check(kq::state_dump(kq::Store(kq::memory_files(files)).load("x")) == kq::state_dump(st), v, "after edits and a drop");

    // A conversation removed takes its files.
    size_t before = files.size();
    st.chats.pop_back();
    Changes rm;
    rm.removed.insert("b");
    s.commit(st, rm);
    check(files.size() == before - 2, v, "removed: record and journal gone");
    check(kq::state_dump(kq::Store(kq::memory_files(files)).load("x")) == kq::state_dump(st), v, "after a removal");

    // A broken record leaves that conversation out and says so.
    for (auto& [n, dd] : files)
        if (n.size() > 5 && n.substr(n.size() - 5) == ".json" && n != "state.json") dd = "{\"id\":";
    problems.clear();
    State broken = kq::Store(kq::memory_files(files)).load("x", &problems);
    check(broken.chats.empty() && problems.size() == 1, v, "a broken record is reported, not loaded");

    // The same on disk.
    std::string dir = "build/store-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    State disk{"tdeck", {chat("on disk", 5)}};
    kq::Store p(kq::posix_files(dir));
    check(p.commit(disk, all(disk)), v, "posix commit");
    check(kq::state_dump(kq::Store(kq::posix_files(dir)).load("x")) == kq::state_dump(disk), v, "posix load");
    check(!std::filesystem::exists(dir + "/state.json.new"), v, "no temporary left");
    return finish("store");
}
