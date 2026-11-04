// SPDX-License-Identifier: AGPL-3.0-only
// What the screens and the CLI show of the chats, copied out of the engine under its lock, and
// the commands they send back. Any task.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct KqLine {
    enum Mark { NONE, PENDING, WRITTEN, READ } mark = NONE;  // NONE on others' lines
    bool notice = false, mine = false;
    std::string who, text;
};

struct KqChat {
    std::string id;
    bool joined = false, adding = false;
    std::string voucher;  // ours to give out, while waiting to be added; "…" while minting
    unsigned members = 0, pending = 0;
    std::vector<KqLine> lines;  // only when asked for
};

// Changes whenever a chat does; a screen redraws when it moves.
uint32_t kqGeneration();
// Every chat, or only `only`; with its lines when `lines`.
std::vector<KqChat> kqChats(const std::string& only = "", bool lines = false);
// create, join, scan, leave: group; invite: group and voucher code; send: group and text.
void kqCommand(const char* verb, const std::string& group, const std::string& arg = "");

// Whether the chats can be read yet: at first start the user chooses whether a passphrase
// protects them; with one, they stay locked after every start until it is given.
enum class KqLock { STARTING, UNDECIDED, LOCKED, UNLOCKING, OPEN, LOST };
// The state, and for UNDECIDED, LOCKED and LOST why, or what went wrong last.
KqLock kqLock(std::string* why = nullptr);
// At first start: protect the chats with this passphrase, or with none when it is empty.
void kqSetup(const std::string& passphrase);
void kqUnlock(const std::string& passphrase);
