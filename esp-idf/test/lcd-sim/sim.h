#pragma once
#include <string>
void simClientStart(const std::string& socketPath, const std::string& name, size_t boxSize, const std::string& stateDir);
std::string simSaved();
// The talk key held (or let go), as lxst would report it; the sim records and sends nothing.
void simTalk(bool down);
bool simChannelUp();
