// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_json.h"

#include <cctype>

namespace kq {

const Json& Json::operator[](const std::string& k) const {
    static const Json none;
    auto it = o.find(k);
    return it == o.end() ? none : it->second;
}

namespace {

struct Parser {
    const std::string& t;
    size_t i = 0;
    int depth = 0;

    [[noreturn]] void bad(const char* what) { throw JsonError(std::string("json: ") + what + " at " + std::to_string(i)); }
    char peek() { return i < t.size() ? t[i] : '\0'; }
    void ws() { while (i < t.size() && isspace((unsigned char)t[i])) i++; }
    void want(char c) { ws(); if (peek() != c) bad("unexpected"); i++; }
    bool word(const char* w, size_t n) { if (t.compare(i, n, w) != 0) return false; i += n; return true; }

    void utf8(std::string& out, unsigned c) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | c >> 6); out += (char)(0x80 | (c & 63)); }
        else { out += (char)(0xE0 | c >> 12); out += (char)(0x80 | (c >> 6 & 63)); out += (char)(0x80 | (c & 63)); }
    }

    std::string string() {
        want('"');
        std::string s;
        for (;;) {
            if (i >= t.size()) bad("unterminated string");
            char c = t[i++];
            if (c == '"') return s;
            if (c != '\\') { s += c; continue; }
            if (i >= t.size()) bad("unterminated escape");
            switch (char e = t[i++]) {
                case '"': case '\\': case '/': s += e; break;
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case 'r': s += '\r'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'u': {
                    if (i + 4 > t.size()) bad("short \\u");
                    unsigned c4 = (unsigned)std::stoul(t.substr(i, 4), nullptr, 16);
                    i += 4;
                    utf8(s, c4);  // what dump() writes: control characters only
                    break;
                }
                default: bad("unknown escape");
            }
        }
    }

    Json value() {
        if (++depth > 64) bad("too deep");
        ws();
        Json j;
        char c = peek();
        if (c == '{') {
            j.kind = Json::OBJ;
            i++;
            ws();
            if (peek() == '}') i++;
            else for (;;) {
                std::string k = string();
                want(':');
                j.o[k] = value();
                ws();
                if (peek() == '}') { i++; break; }
                want(',');
            }
        } else if (c == '[') {
            j.kind = Json::ARR;
            i++;
            ws();
            if (peek() == ']') i++;
            else for (;;) {
                j.a.push_back(value());
                ws();
                if (peek() == ']') { i++; break; }
                want(',');
            }
        } else if (c == '"') {
            j.kind = Json::STR;
            j.s = string();
        } else if (word("null", 4)) {
        } else if (word("true", 4)) { j.kind = Json::BOOL; j.s = "true";
        } else if (word("false", 5)) { j.kind = Json::BOOL; j.s = "false";
        } else {
            j.kind = Json::NUM;
            if (peek() == '-') j.s += t[i++];
            while (isdigit((unsigned char)peek())) j.s += t[i++];
            if (j.s.empty() || j.s == "-") bad("not a value");
        }
        depth--;
        return j;
    }
};

void quote(std::string& out, const std::string& s) {
    static const char* d = "0123456789abcdef";
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c == '\n') out += "\\n";
        else if (c < 0x20) { out += "\\u00"; out += d[c >> 4]; out += d[c & 15]; }
        else out += (char)c;
    }
    out += '"';
}

void dump_to(std::string& out, const Json& j) {
    switch (j.kind) {
        case Json::NUL: out += "null"; break;
        case Json::BOOL: case Json::NUM: out += j.s; break;
        case Json::STR: quote(out, j.s); break;
        case Json::ARR:
            out += '[';
            for (size_t i = 0; i < j.a.size(); i++) { if (i) out += ','; dump_to(out, j.a[i]); }
            out += ']';
            break;
        case Json::OBJ: {
            out += '{';
            bool first = true;
            for (auto& [k, v] : j.o) {
                if (!first) out += ',';
                first = false;
                quote(out, k);
                out += ':';
                dump_to(out, v);
            }
            out += '}';
        }
    }
}

}  // namespace

Json Json::parse(const std::string& t) {
    Parser p{t};
    Json j = p.value();
    p.ws();
    if (p.i != t.size()) p.bad("trailing bytes");
    return j;
}

std::string Json::dump() const {
    std::string s;
    dump_to(s, *this);
    return s;
}

}  // namespace kq
