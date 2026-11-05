// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_replica.h"

#include <algorithm>

namespace kq {

static const uint8_t* ctx() { return (const uint8_t*)bacap::kPigeonholeCtx; }
static constexpr size_t kCtxLen = sizeof bacap::kPigeonholeCtx - 1;

bool ReplicaClient::write(const bacap::WriteCap& w, const bacap::Index& idx, const Bytes& plaintext, WriteDone done) {
    auto padded = wire::pad(plaintext.data(), plaintext.size(), box_size_);
    if (!padded) return false;
    auto s = bacap::encrypt(w, idx, ctx(), kCtxLen, padded->data(), padded->size());
    Req r;
    r.write = true;
    r.query = wire::write_query(s.box_id.data(), s.signature.data(), s.ciphertext.data(), s.ciphertext.size());
    r.on_reply = [done](const std::optional<wire::Reply>& rep) { done(rep ? rep->code : (uint8_t)wire::INTERNAL_ERROR); };
    enqueue(std::move(r));
    return true;
}

void ReplicaClient::tombstone(const bacap::WriteCap& w, const bacap::Index& idx, WriteDone done) {
    auto id = bacap::box_id(w.root_pk, idx, ctx(), kCtxLen);
    auto sig = bacap::sign_box(w, idx, ctx(), kCtxLen, nullptr, 0);
    Req r;
    r.write = true;
    r.query = wire::write_query(id->data(), sig.data(), nullptr, 0);
    r.on_reply = [done](const std::optional<wire::Reply>& rep) { done(rep ? rep->code : (uint8_t)wire::INTERNAL_ERROR); };
    enqueue(std::move(r));
}

void ReplicaClient::read(const uint8_t root_pk[32], const bacap::Index& idx, ReadDone done) {
    auto id = bacap::box_id(root_pk, idx, ctx(), kCtxLen);
    if (!id) { done(ReadOutcome{}); return; }
    Req r;
    r.query = wire::read_query(id->data());
    auto box = *id;
    r.on_reply = [done, idx, box](const std::optional<wire::Reply>& rep) {
        ReadOutcome o;
        if (rep && rep->code == wire::NOT_FOUND) o.kind = ReadOutcome::NOT_FOUND;
        else if (rep && rep->code == wire::TOMBSTONE) o.kind = ReadOutcome::TOMBSTONE;
        else if (rep && rep->found) {
            auto pt = bacap::decrypt(idx, box.data(), ctx(), kCtxLen, rep->ciphertext.data(), rep->ciphertext.size(),
                                     rep->signature.data());
            if (pt && pt->empty()) o.kind = ReadOutcome::TOMBSTONE;
            else if (pt) {
                auto un = wire::unpad(pt->data(), pt->size());
                if (un) { o.kind = ReadOutcome::FOUND; o.plaintext = std::move(*un); }
            }
        }
        done(o);
    };
    enqueue(std::move(r));
}

void ReplicaClient::enqueue(Req r) {
    queue_.push_back(std::move(r));
    if (up_) pump(now_);
}

size_t ReplicaClient::flying(bool write) const {
    size_t n = 0;
    for (auto& [_, r] : in_flight_) n += r.write == write;
    return n;
}

// The first queued request of each kind goes out while its kind has room, in order within a kind.
void ReplicaClient::pump(uint32_t now_ms) {
    while (up_) {
        auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Req& q) { return flying(q.write) < kMaxInFlight; });
        if (it == queue_.end()) return;
        Req r = std::move(*it);
        queue_.erase(it);
        uint16_t id = next_id_++;
        if (next_id_ == 0) next_id_ = 1;
        auto frames = wire::split(id, r.query.data(), r.query.size(), piece_);
        bool ok = !frames.empty();
        for (auto& f : frames) ok = ok && send_(f.pack());
        if (!ok) { r.on_reply(std::nullopt); continue; }
        r.sent_at = now_ms;
        in_flight_.emplace(id, std::move(r));
    }
}

void ReplicaClient::on_message(const uint8_t* p, size_t n, uint32_t now_ms) {
    now_ = now_ms;
    auto f = wire::Frame::unpack(p, n);
    if (!f) return;
    auto data = reasm_.feed(*f, now_ms);
    if (!data) return;
    auto it = in_flight_.find(f->request_id);
    if (it == in_flight_.end()) return;  // late reply to a request already failed
    Req r = std::move(it->second);
    in_flight_.erase(it);
    r.on_reply(wire::decode_reply(data->data(), data->size()));
    pump(now_ms);
}

void ReplicaClient::tick(uint32_t now_ms) {
    now_ = now_ms;
    for (auto it = in_flight_.begin(); it != in_flight_.end();) {
        if (now_ms - it->second.sent_at >= kTimeoutMs) {
            Req r = std::move(it->second);
            it = in_flight_.erase(it);
            r.on_reply(std::nullopt);
        } else {
            ++it;
        }
    }
    pump(now_ms);
}

void ReplicaClient::ready(uint32_t now_ms) {
    up_ = true;
    now_ = now_ms;
    pump(now_ms);
}

void ReplicaClient::reset() {
    up_ = false;
    reasm_ = wire::Reassembler();
    auto flying = std::move(in_flight_);
    auto queued = std::move(queue_);
    in_flight_.clear();
    queue_.clear();
    for (auto& [id, r] : flying) r.on_reply(std::nullopt);
    for (auto& r : queued) r.on_reply(std::nullopt);
}

}  // namespace kq
