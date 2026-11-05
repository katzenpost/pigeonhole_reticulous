// SPDX-License-Identifier: AGPL-3.0-only
// What one box costs at the replica: n rounds, one request at a time, each a write of a fresh
// box, a read of it (a hit) and a read of the index after it (a miss, which is what a poll is).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "kq_replica.h"

namespace kq {

class Probe {
public:
    using Clock = std::function<uint32_t()>;  // milliseconds
    struct Times {
        std::vector<uint32_t> ms;
        unsigned failed = 0;
        std::string line(const char* what) const;  // "write 10: median 412 ms, min 380, max 520"
    };

    Probe(ReplicaClient& c, Clock now, bacap::Rng rng) : c_(c), now_(std::move(now)), rng_(std::move(rng)) {}
    // false while one runs. done gets status() once the last round ends.
    bool start(unsigned rounds, std::function<void(const std::string&)> done = {});
    bool running() const { return left_ > 0; }
    std::string status() const;
    Times write, hit, miss;

private:
    void round();
    ReplicaClient& c_;
    Clock now_;
    bacap::Rng rng_;
    unsigned left_ = 0, rounds_ = 0;
    std::function<void(const std::string&)> done_;
};

}  // namespace kq
