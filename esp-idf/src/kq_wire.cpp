#include "kq_wire.h"

#include <cstring>

namespace kq::wire {

std::optional<Bytes> pad(const uint8_t* data, size_t len, size_t box_size) {
    if (len > box_size) return std::nullopt;
    Bytes out(box_size + 4, 0);
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(len >> (24 - 8 * i));
    if (len) memcpy(out.data() + 4, data, len);
    return out;
}

std::optional<Bytes> unpad(const uint8_t* p, size_t len) {
    if (len < 4) return std::nullopt;
    uint32_t n = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    if (n > len - 4) return std::nullopt;
    return Bytes(p + 4, p + 4 + n);
}

Bytes read_query(const uint8_t box_id[32]) {
    Bytes q{0};
    q.insert(q.end(), box_id, box_id + 32);
    return q;
}

Bytes write_query(const uint8_t box_id[32], const uint8_t sig[64], const uint8_t* ct, size_t ct_len) {
    Bytes q{1};
    q.insert(q.end(), box_id, box_id + 32);
    q.insert(q.end(), sig, sig + 64);
    q.insert(q.end(), ct, ct + ct_len);
    return q;
}

std::optional<Reply> decode_reply(const uint8_t* p, size_t len) {
    if (len < 1) return std::nullopt;
    Reply r;
    r.code = p[0];
    if (r.code == SUCCESS && len > 1) {
        if (len < 1 + 64) return std::nullopt;
        r.found = true;
        memcpy(r.signature.data(), p + 1, 64);
        r.ciphertext.assign(p + 65, p + len);
    }
    return r;
}

Bytes Frame::pack() const {
    Bytes b{(uint8_t)(request_id >> 8), (uint8_t)request_id, index, count};
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

std::optional<Frame> Frame::unpack(const uint8_t* p, size_t len) {
    if (len < kFrameHeader) return std::nullopt;
    Frame f;
    f.request_id = (uint16_t)(p[0] << 8 | p[1]);
    f.index = p[2];
    f.count = p[3];
    f.data.assign(p + kFrameHeader, p + len);
    return f;
}

std::vector<Frame> split(uint16_t request_id, const uint8_t* data, size_t len, size_t piece) {
    size_t count = len == 0 ? 1 : (len + piece - 1) / piece;
    if (piece == 0 || count > kMaxPieces) return {};
    std::vector<Frame> out(count);
    for (size_t i = 0; i < count; i++) {
        size_t off = i * piece, n = len - off < piece ? len - off : piece;
        out[i].request_id = request_id;
        out[i].index = (uint8_t)i;
        out[i].count = (uint8_t)count;
        out[i].data.assign(data + off, data + off + (len ? n : 0));
    }
    return out;
}

std::optional<Bytes> Reassembler::feed(const Frame& f, uint32_t now) {
    for (auto it = pending_.begin(); it != pending_.end();)
        it = now - it->second.started >= timeout_ms_ ? pending_.erase(it) : std::next(it);
    if (f.count == 0 || f.index >= f.count) {
        pending_.erase(f.request_id);
        return std::nullopt;
    }
    auto [it, fresh] = pending_.try_emplace(f.request_id, Pending{now, f.count, {}});
    if (!fresh && it->second.count != f.count) {
        pending_.erase(it);
        return std::nullopt;
    }
    it->second.pieces.emplace(f.index, f.data);
    if (it->second.pieces.size() < it->second.count) return std::nullopt;
    Bytes out;
    for (auto& [i, d] : it->second.pieces) out.insert(out.end(), d.begin(), d.end());
    pending_.erase(it);
    return out;
}

}  // namespace kq::wire
