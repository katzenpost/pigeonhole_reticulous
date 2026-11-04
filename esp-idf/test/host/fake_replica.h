// SPDX-License-Identifier: AGPL-3.0-only
// An in-memory replica that answers as pigeonhole_rns does: stores verified boxes, refuses a
// different box under a used ID, never replaces a tombstone.
#pragma once
#include <cstring>
#include <map>
#include <vector>

#include "kq_replica.h"

using namespace kq;

struct FakeReplica {
    std::map<std::array<uint8_t, 32>, std::pair<Bytes, Bytes>> boxes;  // id -> (sig, ct); empty ct a tombstone
    wire::Reassembler reasm;
    std::vector<Bytes> outbox;  // frames for the client
    bool answer = true;

    void on_frame(const Bytes& f) {
        auto fr = wire::Frame::unpack(f.data(), f.size());
        auto q = reasm.feed(*fr, 0);
        if (!q || !answer) return;
        Bytes reply;
        std::array<uint8_t, 32> id;
        memcpy(id.data(), q->data() + 1, 32);
        if ((*q)[0] == 0) {
            auto it = boxes.find(id);
            if (it == boxes.end()) reply = {wire::NOT_FOUND};
            else if (it->second.second.empty()) reply = {wire::TOMBSTONE};
            else {
                reply = {wire::SUCCESS};
                reply.insert(reply.end(), it->second.first.begin(), it->second.first.end());
                reply.insert(reply.end(), it->second.second.begin(), it->second.second.end());
            }
        } else {
            Bytes sig(q->begin() + 33, q->begin() + 97), ct(q->begin() + 97, q->end());
            auto it = boxes.find(id);
            if (!ed_verify(id.data(), ct.data(), ct.size(), sig.data())) reply = {wire::INVALID_SIGNATURE};
            else if (it != boxes.end() && it->second.second.empty()) reply = {wire::ALREADY_EXISTS};
            else if (it != boxes.end() && !ct.empty() && it->second != std::make_pair(sig, ct)) reply = {wire::ALREADY_EXISTS};
            else { boxes[id] = {sig, ct}; reply = {wire::SUCCESS}; }
        }
        for (auto& r : wire::split(fr->request_id, reply.data(), reply.size(), 900)) outbox.push_back(r.pack());
    }
};

