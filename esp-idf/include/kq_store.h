// SPDX-License-Identifier: AGPL-3.0-only
// The client's state on flash, as the Rust client's store keeps it: per conversation a record
// (everything but its lines, rewritten when it changes) and its lines, kept as a journal that
// each step appends to; and which conversations there are, in order. A step's changes are saved
// before its actions run, so a box index once used is never handed out again after a restart.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>

#include "kq1_engine.h"

namespace kq {

// The files of one directory, by name.
struct StoreFiles {
    std::function<std::optional<std::string>(const std::string& name)> read;
    std::function<bool(const std::string& name, const std::string& data)> replace;  // atomically, synced
    std::function<bool(const std::string& name, const std::string& data)> append;   // synced
    std::function<void(const std::string& name)> remove;
};
// StoreFiles on a POSIX directory, which must exist.
StoreFiles posix_files(const std::string& dir);
// StoreFiles in memory, for tests.
StoreFiles memory_files(std::map<std::string, std::string>& files);
// Everything saved of a state, as text: two states that save alike dump alike.
std::string state_dump(const kq1::State& st);

class Store {
public:
    explicit Store(StoreFiles f) : f_(std::move(f)) {}
    // What was saved, with `me` when nothing was. A conversation that cannot be read is left
    // out and named in problems.
    kq1::State load(const std::string& me, std::vector<std::string>* problems = nullptr);
    // Saves what the changes name; false when a write failed.
    bool commit(const kq1::State& st, const kq1::Changes& ch);

    size_t writes = 0, bytes = 0;  // since start

private:
    bool put(const std::string& name, const std::string& data, bool whole);
    bool save_log(const kq1::Chat& c);
    StoreFiles f_;
    std::string meta_;
    std::map<kq1::ChatId, std::string> saved_;  // each record as last written
    std::map<kq1::ChatId, size_t> entries_;     // journal entries per conversation
};

}  // namespace kq
