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

// A small JSON value for nested vector fields: objects compare regardless of key order,
// numbers are kept as their digits so 64-bit values stay exact.
struct Json {
    enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
    std::string s;  // NUM digits, STR text, BOOL "true"/"false"
    std::vector<Json> a;
    std::map<std::string, Json> o;

    static Json num(unsigned long long v) { Json j; j.kind = NUM; j.s = std::to_string(v); return j; }
    static Json str(const std::string& v) { Json j; j.kind = STR; j.s = v; return j; }
    static Json arr(std::vector<Json> v) { Json j; j.kind = ARR; j.a = std::move(v); return j; }
    static Json obj(std::map<std::string, Json> v) { Json j; j.kind = OBJ; j.o = std::move(v); return j; }
    bool operator==(const Json& x) const { return kind == x.kind && s == x.s && a == x.a && o == x.o; }
    bool is_null() const { return kind == NUL; }
    const Json& operator[](const std::string& k) const {
        static Json none;
        auto it = o.find(k);
        return it == o.end() ? none : it->second;
    }
    unsigned long long u() const { return std::stoull(s); }

    static Json parse(const std::string& t) { size_t i = 0; return value(t, i); }
    static void ws(const std::string& t, size_t& i) { while (i < t.size() && isspace((unsigned char)t[i])) i++; }
    static Json value(const std::string& t, size_t& i) {
        ws(t, i);
        Json j;
        char c = t[i];
        if (c == '{') {
            j.kind = OBJ;
            i++;
            ws(t, i);
            if (t[i] == '}') { i++; return j; }
            for (;;) {
                Json k = value(t, i);
                ws(t, i);
                i++;  // ':'
                j.o[k.s] = value(t, i);
                ws(t, i);
                if (t[i++] == '}') return j;
            }
        }
        if (c == '[') {
            j.kind = ARR;
            i++;
            ws(t, i);
            if (t[i] == ']') { i++; return j; }
            for (;;) {
                j.a.push_back(value(t, i));
                ws(t, i);
                if (t[i++] == ']') return j;
            }
        }
        if (c == '"') {
            j.kind = STR;
            for (i++; t[i] != '"'; i++) {
                if (t[i] == '\\') i++;  // vectors carry hex and names: no \u escapes needed
                j.s += t[i];
            }
            i++;
            return j;
        }
        if (t.compare(i, 4, "null") == 0) { i += 4; return j; }
        if (t.compare(i, 4, "true") == 0) { i += 4; j.kind = BOOL; j.s = "true"; return j; }
        if (t.compare(i, 5, "false") == 0) { i += 5; j.kind = BOOL; j.s = "false"; return j; }
        j.kind = NUM;
        while (i < t.size() && (isdigit((unsigned char)t[i]) || t[i] == '-')) j.s += t[i++];
        return j;
    }
};

inline kq::Bytes unhex(const std::string& h) {
    kq::Bytes b;
    for (size_t i = 0; i + 1 < h.size(); i += 2) b.push_back((uint8_t)std::stoi(h.substr(i, 2), nullptr, 16));
    return b;
}
inline std::string tohex(const kq::Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}
