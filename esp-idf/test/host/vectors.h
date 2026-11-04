// Reads build/vectors.txt (see gen_vectors.py) and counts checks.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "kq_crypto.h"

struct Vec {
    std::string file, name;
    std::map<std::string, std::string> f;
    const std::string& s(const std::string& k) const {
        auto it = f.find(k);
        if (it == f.end()) { fprintf(stderr, "%s/%s: no field %s\n", file.c_str(), name.c_str(), k.c_str()); exit(2); }
        return it->second;
    }
    bool has(const std::string& k) const { return f.count(k) != 0; }
    kq::Bytes hex(const std::string& k) const {
        const std::string& h = s(k);
        kq::Bytes b;
        for (size_t i = 0; i + 1 < h.size(); i += 2) b.push_back((uint8_t)std::stoi(h.substr(i, 2), nullptr, 16));
        return b;
    }
    unsigned long long u(const std::string& k) const { return std::stoull(s(k)); }
    bool b(const std::string& k) const { return s(k) == "true"; }
};

inline std::vector<Vec> load(const char* path, const std::string& file) {
    std::ifstream in(path);
    std::vector<Vec> out;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> parts;
        size_t p = 0, q;
        while ((q = line.find('\t', p)) != std::string::npos) { parts.push_back(line.substr(p, q - p)); p = q + 1; }
        parts.push_back(line.substr(p));
        if (parts[0] != file) continue;
        Vec v{parts[0], parts[1], {}};
        for (size_t i = 2; i < parts.size(); i++) {
            size_t e = parts[i].find('=');
            v.f[parts[i].substr(0, e)] = parts[i].substr(e + 1);
        }
        out.push_back(v);
    }
    if (out.empty()) { fprintf(stderr, "no vectors for %s\n", file.c_str()); exit(2); }
    return out;
}

inline int g_checks = 0, g_fail = 0;
inline void check(bool ok, const Vec& v, const char* what) {
    g_checks++;
    if (!ok) { g_fail++; fprintf(stderr, "FAIL %s/%s: %s\n", v.file.c_str(), v.name.c_str(), what); }
}
inline int finish(const char* suite) {
    printf("%s: %d checks, %d failed\n", suite, g_checks, g_fail);
    return g_fail ? 1 : 0;
}
