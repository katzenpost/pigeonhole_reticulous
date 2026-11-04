#include "kq_cbor.h"

#include <cstring>

namespace kq::cbor {

void Writer::head(uint8_t major, uint64_t v)
{
    uint8_t m = (uint8_t)(major << 5);
    if (v < 24) { out.push_back(m | (uint8_t)v); return; }
    int n;
    if (v <= 0xff)              { out.push_back(m | 24); n = 1; }
    else if (v <= 0xffff)       { out.push_back(m | 25); n = 2; }
    else if (v <= 0xffffffffu)  { out.push_back(m | 26); n = 4; }
    else                        { out.push_back(m | 27); n = 8; }
    for (int i = n - 1; i >= 0; --i) out.push_back((uint8_t)(v >> (8 * i)));
}

const Value* Value::at(uint64_t key) const
{
    if (kind != MAP) return nullptr;
    for (size_t i = 0; i + 1 < items.size(); i += 2)
        if (items[i].kind == UINT && items[i].u == key) return &items[i + 1];
    return nullptr;
}

const Value* Value::atText(const char* key) const
{
    if (kind != MAP) return nullptr;
    for (size_t i = 0; i + 1 < items.size(); i += 2)
        if (items[i].kind == TEXT && items[i].s == key) return &items[i + 1];
    return nullptr;
}

namespace {

struct Reader {
    const uint8_t* p;
    size_t         n;
    size_t         i = 0;

    bool arg(uint8_t info, uint64_t& v)
    {
        if (info < 24) { v = info; return true; }
        int len = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : -1;
        if (len < 0 || i + (size_t)len > n) return false;
        v = 0;
        for (int k = 0; k < len; ++k) v = (v << 8) | p[i++];
        return true;
    }

    /* Indefinite-length byte/text string: definite chunks of the same major
     * type up to a break. */
    bool chunks(uint8_t major, std::string& s)
    {
        for (;;) {
            if (i >= n) return false;
            uint8_t ib = p[i++];
            if (ib == 0xff) return true;
            if ((ib >> 5) != major) return false;
            uint64_t len;
            if (!arg(ib & 0x1f, len) || len > n - i) return false;
            s.append((const char*)p + i, (size_t)len);
            i += (size_t)len;
        }
    }

    bool item(Value& v, int depth)
    {
        if (depth > 16 || i >= n) return false;
        uint8_t ib = p[i++];
        uint8_t major = ib >> 5, info = ib & 0x1f;
        uint64_t a = 0;
        if (info == 31) {   /* indefinite */
            switch (major) {
            case 2: v.kind = Value::BYTES; return chunks(2, v.s);
            case 3: v.kind = Value::TEXT;  return chunks(3, v.s);
            case 4: case 5: {
                v.kind = major == 4 ? Value::ARRAY : Value::MAP;
                for (;;) {
                    if (i >= n) return false;
                    if (p[i] == 0xff) { i++; break; }
                    Value e;
                    if (!item(e, depth + 1)) return false;
                    v.items.push_back(std::move(e));
                }
                return major == 4 || v.items.size() % 2 == 0;
            }
            default: return false;
            }
        }
        if (major != 7 && !arg(info, a)) return false;
        switch (major) {
        case 0: v.kind = Value::UINT; v.u = a; return true;
        case 1: v.kind = Value::NINT; v.u = a; return true;
        case 2: case 3:
            if (a > n - i) return false;
            v.kind = major == 2 ? Value::BYTES : Value::TEXT;
            v.s.assign((const char*)p + i, (size_t)a);
            i += (size_t)a;
            return true;
        case 4: case 5: {
            v.kind = major == 4 ? Value::ARRAY : Value::MAP;
            uint64_t count = major == 4 ? a : a * 2;
            if (count > n - i) return false;   /* every item is at least a byte */
            v.items.resize((size_t)count);
            for (uint64_t k = 0; k < count; ++k)
                if (!item(v.items[(size_t)k], depth + 1)) return false;
            return true;
        }
        case 6:   /* tag: keep the tagged item, drop the tag */
            return item(v, depth + 1);
        default:  /* 7: simple values and floats */
            if (info == 20 || info == 21) { v.kind = Value::BOOL; v.b = info == 21; return true; }
            if (info == 22 || info == 23) { v.kind = Value::NUL; return true; }
            {
                int len = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 0;
                if (info >= 28) return false;
                if (i + (size_t)len > n) return false;
                i += (size_t)len;
                v.kind = Value::OTHER;
                return true;
            }
        }
    }
};

}  // namespace

bool decode(const uint8_t* p, size_t n, Value& out)
{
    Reader r{ p, n };
    out = Value{};
    if (!r.item(out, 0)) return false;
    return r.i == n;
}

}  // namespace kq::cbor
