#pragma once
#include <string>
void simClientStart(const std::string& socketPath, const std::string& name, size_t boxSize, const std::string& stateDir);
std::string simSaved();
bool simChannelUp();
