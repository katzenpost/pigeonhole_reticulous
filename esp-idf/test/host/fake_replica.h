// SPDX-License-Identifier: AGPL-3.0-only
// An in-memory replica that answers as pigeonhole_rns does: stores verified boxes, refuses a
// different box under a used ID, never replaces a tombstone. With push it takes subscriptions
// (op 6) and pushes a box to them once it holds it; without, it refuses op 6 as an old one does.
#pragma once
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "kq_replica.h"

using namespace kq;

struct FakeReplica {
    std::map<std::array<uint8_t, 32>, std::pair<Bytes, Bytes>> boxes;  // id -> (sig, ct); empty ct a tombstone
    wire::Reassembler reasm;
    std::vector<Bytes> outbox;  // frames for the client
    std::vector<Bytes> pushbox;  // push frames for the client (MSGTYPE wire::kPushMsgType)
    bool answer = true, push = false;
    std::set<std::array<uint8_t, 32>> subs;
    uint16_t push_id = 0;
    size_t reads = 0;  // read queries answered

    Bytes reply_of(const std::array<uint8_t, 32>& id) const {
        auto it = boxes.find(id);
        if (it == boxes.end()) return {wire::NOT_FOUND};
        if (it->second.second.empty()) return {wire::TOMBSTONE};
        Bytes reply{wire::SUCCESS};
        reply.insert(reply.end(), it->second.first.begin(), it->second.first.end());
        reply.insert(reply.end(), it->second.second.begin(), it->second.second.end());
        return reply;
    }
    void pushed(const std::array<uint8_t, 32>& id) {
        if (!subs.erase(id)) return;
        Bytes msg(id.begin(), id.end());
        Bytes r = reply_of(id);
        msg.insert(msg.end(), r.begin(), r.end());
        for (auto& f : wire::split(push_id++, msg.data(), msg.size(), wire::piece_size(500))) pushbox.push_back(f.pack());
    }

    void on_frame(const Bytes& f) {
        auto fr = wire::Frame::unpack(f.data(), f.size());
        auto q = reasm.feed(*fr, 0);
        if (!q || !answer) return;
        Bytes reply;
        std::array<uint8_t, 32> id;
        uint8_t op = (*q)[0];
        if (op == wire::kSubscribe) {
            if (!push || q->size() != 3 + 32) {
                reply = {wire::INVALID_PAYLOAD};
            } else {
                memcpy(id.data(), q->data() + 3, 32);
                subs.insert(id);
                reply = {wire::SUCCESS, 0, 1, 0x0e, 0x10};  // 1 taken, for 3600 s
            }
        } else if (op == 0) {
            reads++;
            memcpy(id.data(), q->data() + 1, 32);
            reply = reply_of(id);
        } else {
            memcpy(id.data(), q->data() + 1, 32);
            Bytes sig(q->begin() + 33, q->begin() + 97), ct(q->begin() + 97, q->end());
            auto it = boxes.find(id);
            if (!ed_verify(id.data(), ct.data(), ct.size(), sig.data())) reply = {wire::INVALID_SIGNATURE};
            else if (it != boxes.end() && it->second.second.empty()) reply = {wire::ALREADY_EXISTS};
            else if (it != boxes.end() && !ct.empty() && it->second != std::make_pair(sig, ct)) reply = {wire::ALREADY_EXISTS};
            else { boxes[id] = {sig, ct}; reply = {wire::SUCCESS}; }
        }
        for (auto& r : wire::split(fr->request_id, reply.data(), reply.size(), 900)) outbox.push_back(r.pack());
        // A box already held is pushed at once, after the subscribe's reply.
        if (op == wire::kSubscribe && reply[0] == wire::SUCCESS && boxes.count(id)) pushed(id);
        if (op == 1 && reply[0] == wire::SUCCESS) pushed(id);
    }
};
