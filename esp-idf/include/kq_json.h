// SPDX-License-Identifier: AGPL-3.0-only
// A small JSON value: objects compare regardless of key order, numbers are kept as their digits
// so 64-bit values stay exact. The traces of katzenqt1's member and the client's saved state.
#pragma once
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace kq {

struct JsonError : std::runtime_error { using std::runtime_error::runtime_error; };

struct Json {
    enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
    std::string s;  // NUM digits, STR text, BOOL "true"/"false"
    std::vector<Json> a;
    std::map<std::string, Json> o;

    static Json num(unsigned long long v) { Json j; j.kind = NUM; j.s = std::to_string(v); return j; }
    static Json neg(long long v) { Json j; j.kind = NUM; j.s = std::to_string(v); return j; }
    static Json boolean(bool v) { Json j; j.kind = BOOL; j.s = v ? "true" : "false"; return j; }
    static Json str(const std::string& v) { Json j; j.kind = STR; j.s = v; return j; }
    static Json arr(std::vector<Json> v) { Json j; j.kind = ARR; j.a = std::move(v); return j; }
    static Json obj(std::map<std::string, Json> v) { Json j; j.kind = OBJ; j.o = std::move(v); return j; }
    bool operator==(const Json& x) const { return kind == x.kind && s == x.s && a == x.a && o == x.o; }
    bool is_null() const { return kind == NUL; }
    bool b() const { return kind == BOOL && s == "true"; }
    unsigned long long u() const { return std::stoull(s); }
    const Json& operator[](const std::string& k) const;

    // Throws JsonError on anything but one whole JSON value.
    static Json parse(const std::string& t);
    std::string dump() const;
};

}  // namespace kq
