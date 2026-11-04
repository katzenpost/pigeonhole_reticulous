// SPDX-License-Identifier: AGPL-3.0-only
// The first start's choice and the sealed state: nothing said in a group is left readable in
// a file, only the passphrase opens it, chats kept before are sealed in place, a lost lock is
// not replaced, and a changed or torn file is caught.
#include <sodium.h>

#include "kq_vault.h"
#include "vectors.h"

using namespace kq1;

static const std::string kWords = "the words of a message", kName = "friends of kate";

static State state() {
    Chat c;
    c.id = c.name = kName;
    c.write_cap = Bytes(32, 7);
    c.next_write = Bytes(104, 9);
    Line l;
    l.id = c.fresh();
    l.sender = Bytes(32, 1);
    l.index = Bytes(104, 2);
    l.text = kWords;
    c.log.push_back(l);
    return State{"tdeck", {c}};
}

static bool commit_all(kq::Store& s, const State& st) {
    Changes ch;
    for (auto& c : st.chats) {
        ch.chats.insert(c.id);
        for (auto& l : c.log) ch.lines[c.id].insert(l.id);
    }
    return s.commit(st, ch);
}

static bool holds(const std::map<std::string, std::string>& files, const std::string& needle) {
    for (auto& [n, d] : files)
        if (d.find(needle) != std::string::npos || n.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    if (sodium_init() < 0) return 2;
    kq::Vault::ops = 1;
    kq::Vault::mem = 1 << 16;
    Vec v{"vault", "", {}};
    const std::string want = kq::state_dump(state());

    {  // No passphrase: kept as they are, and asked once only.
        std::map<std::string, std::string> files;
        kq::Vault a(kq::memory_files(files));
        check(a.state() == KqLock::UNDECIDED, v, "fresh: undecided");
        check(!a.serve(), v, "nothing to open before the choice");
        a.ask_setup("");
        check(a.serve() && a.state() == KqLock::OPEN, v, "no passphrase: open");
        kq::Store s(a.files());
        commit_all(s, state());
        check(holds(files, kWords), v, "no passphrase: the words are in the file");
        kq::Vault b(kq::memory_files(files));
        check(b.state() == KqLock::OPEN && b.serve(), v, "no passphrase: open at the next start");
        check(kq::state_dump(kq::Store(b.files()).load("x")) == want, v, "no passphrase: loads");
    }

    {  // A passphrase: nothing readable, and only it opens.
        std::map<std::string, std::string> files;
        kq::Vault a(kq::memory_files(files));
        a.ask_setup("correct horse");
        check(a.serve() && a.state() == KqLock::OPEN, v, "passphrase: open after the choice");
        kq::Store s(a.files());
        check(commit_all(s, state()), v, "sealed commit");
        Line more;
        more.id = 1;
        more.sender = Bytes(32, 1);
        more.index = Bytes(104, 3);
        more.text = "a second line";
        State st = state();
        st.chats[0].log.push_back(more);
        st.chats[0].next_id = 2;
        Changes ch;
        ch.chats.insert(kName);
        ch.lines[kName].insert(1);
        check(s.commit(st, ch), v, "sealed append");
        check(!holds(files, kWords) && !holds(files, "a second line") && !holds(files, "friends") &&
                  !holds(files, "tdeck"),
              v, "passphrase: no words, names or group in any file");

        kq::Vault b(kq::memory_files(files));
        check(b.state() == KqLock::LOCKED && !b.serve(), v, "locked at the next start");
        b.ask_unlock("wrong horse");
        std::string why;
        check(!b.serve() && b.state(&why) == KqLock::LOCKED && !why.empty(), v, "a wrong passphrase stays locked");
        b.ask_unlock("correct horse");
        check(b.serve() && b.state() == KqLock::OPEN, v, "the passphrase opens");
        check(kq::state_dump(kq::Store(b.files()).load("x")) == kq::state_dump(st), v, "sealed: loads as saved");

        // A torn last piece is ignored; a changed piece is caught.
        std::string log;
        for (auto& [n, d] : files) if (n.size() > 6 && n.substr(n.size() - 6) == ".log.s") log = n;
        std::string whole = files[log];
        files[log] = whole + std::string("\0\0\1\0abc", 7);
        std::vector<std::string> problems;
        check(kq::state_dump(kq::Store(b.files()).load("x", &problems)) == kq::state_dump(st) && problems.empty(), v,
              "sealed: a torn last piece is ignored");
        files[log] = whole;
        files[log][whole.size() - 1] ^= 1;
        problems.clear();
        State broken = kq::Store(b.files()).load("x", &problems);
        check(broken.chats.empty() && problems.size() == 1, v, "sealed: a changed piece is caught");
        files[log] = whole;
        // A piece moved to another file does not open there.
        std::string rec;
        for (auto& [n, d] : files) if (n.size() > 7 && n.substr(n.size() - 7) == ".json.s" && n != "state.json.s") rec = n;
        std::string keep = files[rec];
        files[rec] = files[log];
        problems.clear();
        check(kq::Store(b.files()).load("x", &problems).chats.empty() && problems.size() == 1, v, "sealed: pieces are bound to their file");
        files[rec] = keep;

        // The lock gone: not replaced, and not asked again.
        std::string lock = files["lock.json"];
        files.erase("lock.json");
        kq::Vault c(kq::memory_files(files));
        c.ask_setup("");
        check(c.state() == KqLock::LOST && !c.serve(), v, "lost lock: refused");
        check(files.count("state.json.s") && !files.count("lock.json"), v, "lost lock: nothing overwritten");
        files["lock.json"] = lock;
    }

    {  // Chats kept before, unencrypted, are sealed in place at the choice.
        std::map<std::string, std::string> files;
        kq::Store old(kq::memory_files(files));
        commit_all(old, state());
        kq::Vault a(kq::memory_files(files));
        std::string why;
        check(a.state(&why) == KqLock::UNDECIDED && !why.empty(), v, "old chats: undecided, and said so");
        a.ask_setup("correct horse");
        check(a.serve(), v, "old chats: sealed");
        check(!holds(files, kWords) && !files.count("state.json"), v, "old chats: nothing left unencrypted");
        kq::Vault b(kq::memory_files(files));
        b.ask_unlock("correct horse");
        check(b.serve() && kq::state_dump(kq::Store(b.files()).load("x")) == want, v, "old chats: load sealed");
    }

    {  // Cut short after lock.json was written: the unencrypted copy goes at the next unlock.
        std::map<std::string, std::string> files;
        kq::Store old(kq::memory_files(files));
        commit_all(old, state());
        std::map<std::string, std::string> before = files;
        kq::Vault a(kq::memory_files(files));
        a.ask_setup("correct horse");
        a.serve();
        for (auto& [n, d] : before) files[n] = d;  // as if the erase never ran
        kq::Vault b(kq::memory_files(files));
        b.ask_unlock("correct horse");
        check(b.serve() && !holds(files, kWords), v, "an unfinished sealing is finished");
    }

    {  // Fitted to the memory free: less of it, more passes; too little, refused and said so.
        kq::Vault::ops = 3;
        kq::Vault::mem = 4 << 20;
        kq::Vault::room = [] { return (size_t)3 << 20; };
        std::map<std::string, std::string> files;
        kq::Vault a(kq::memory_files(files));
        a.ask_setup("correct horse");
        check(a.serve() && a.last_mem == (2u << 20) && a.last_ops == 6, v, "3 MiB free: 2 MiB, 6 passes");
        kq::Json lock = kq::Json::parse(files["lock.json"]);
        check(lock["mem"].u() == (2u << 20) && lock["ops"].u() == 6, v, "the cost is kept in lock.json");

        kq::Vault::room = [] { return (size_t)1 << 20; };
        kq::Vault b(kq::memory_files(files));
        b.ask_unlock("correct horse");
        std::string why;
        check(!b.serve() && b.state(&why) == KqLock::LOCKED && why.find("memory") != std::string::npos, v,
              "too little free to unlock: still locked, and why");
        kq::Vault::room = [] { return (size_t)-1; };
        b.ask_unlock("correct horse");
        check(b.serve(), v, "unlocks once there is room");

        kq::Vault::room = [] { return (size_t)1 << 20; };
        std::map<std::string, std::string> none;
        kq::Vault c(kq::memory_files(none));
        c.ask_setup("correct horse");
        check(!c.serve() && c.state(&why) == KqLock::UNDECIDED && why.find("No passphrase") != std::string::npos &&
                  !none.count("lock.json"),
              v, "too little free to set up: still undecided, nothing written");
        kq::Vault::room = [] { return (size_t)-1; };
    }

    {  // The device's cost, once, for the time it takes here.
        kq::Vault::ops = 3;
        kq::Vault::mem = 4 << 20;
        std::map<std::string, std::string> files;
        kq::Vault a(kq::memory_files(files));
        a.ask_setup("correct horse");
        check(a.serve() && a.last_mem == (4u << 20) && a.last_ops == 3, v, "at the device's cost");
        printf("vault: Argon2id over 4 MiB, 3 passes: %u ms on this machine\n", a.last_ms);
    }
    return finish("vault");
}
