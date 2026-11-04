/**
 * kq_cbor.h — a small CBOR (RFC 8949) codec, copied from reticulous/rrc (rrc_cbor.h, Apache-2.0).
 *
 * Unsigned and negative integers, byte strings, text strings, arrays, maps,
 * booleans and null. Indefinite lengths, tags and floats are decoded far enough
 * to be skipped, never produced.
 *
 * Plain C++ with no platform includes, so the host test builds it as-is.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace kq::cbor {

/* ── encoding ── */

class Writer {
public:
    std::vector<uint8_t> out;

    void uint(uint64_t v)              { head(0, v); }
    void bytes(const uint8_t* p, size_t n) { head(2, n); out.insert(out.end(), p, p + n); }
    void text(const std::string& s)    { head(3, s.size()); out.insert(out.end(), s.begin(), s.end()); }
    void array(size_t n)               { head(4, n); }
    void map(size_t n)                 { head(5, n); }
    void boolean(bool b)               { out.push_back(b ? 0xf5 : 0xf4); }
    void null()                        { out.push_back(0xf6); }

private:
    void head(uint8_t major, uint64_t v);
};

/* ── decoding ──
 *
 * A decoded value is a tree. Maps keep their entries in wire order; keys are
 * values too, so an integer-keyed and a text-keyed map decode the same way and
 * the caller asks for what it expects (`at(uint)` / `atText(text)`). */

struct Value {
    enum Kind : uint8_t { NONE, UINT, NINT, BYTES, TEXT, ARRAY, MAP, BOOL, NUL, OTHER };
    Kind                 kind = NONE;
    uint64_t             u = 0;        /* UINT; NINT holds -1 - value */
    bool                 b = false;    /* BOOL */
    std::string          s;            /* BYTES and TEXT payload */
    std::vector<Value>   items;        /* ARRAY elements; MAP as key,value,key,value… */

    bool isUint() const  { return kind == UINT; }
    bool isText() const  { return kind == TEXT; }
    bool isBytes() const { return kind == BYTES; }
    bool isMap() const   { return kind == MAP; }
    bool isArray() const { return kind == ARRAY; }
    bool isBool() const  { return kind == BOOL; }

    size_t mapSize() const { return kind == MAP ? items.size() / 2 : 0; }
    /** The value under an unsigned-integer key, or nullptr. */
    const Value* at(uint64_t key) const;
    /** The value under a text key, or nullptr. */
    const Value* atText(const char* key) const;
};

/** Decode one item from `p[0..n)`. Returns false on malformed input, trailing
 *  bytes, or nesting deeper than 16. */
bool decode(const uint8_t* p, size_t n, Value& out);

}  // namespace kq::cbor
