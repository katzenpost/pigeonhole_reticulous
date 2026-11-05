// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_probe.h"

#include <algorithm>

namespace kq {

std::string Probe::Times::line(const char* what) const {
    std::string s = std::string(what) + " " + std::to_string(ms.size());
    if (!ms.empty()) {
        auto v = ms;
        std::sort(v.begin(), v.end());
        s += ": median " + std::to_string(v[v.size() / 2]) + " ms, min " + std::to_string(v.front()) + ", max " +
             std::to_string(v.back());
    }
    if (failed) s += ", " + std::to_string(failed) + " failed";
    return s;
}

bool Probe::start(unsigned rounds, std::function<void(const std::string&)> done) {
    if (running() || rounds == 0) return false;
    write = hit = miss = Times{};
    left_ = rounds_ = rounds;
    done_ = std::move(done);
    round();
    return true;
}

std::string Probe::status() const {
    std::string s = write.line("write") + "; " + hit.line("hit") + "; " + miss.line("miss");
    if (running()) s = "round " + std::to_string(rounds_ - left_ + 1) + "/" + std::to_string(rounds_) + ": " + s;
    return s;
}

void Probe::round() {
    if (left_ == 0) {
        if (done_) done_(status());
        return;
    }
    auto w = bacap::WriteCap::generate(rng_);
    auto r = w.read_cap();
    Bytes text{'k', 'q', ' ', 'p', 'r', 'o', 'b', 'e'};
    uint32_t t0 = now_();
    auto next = [this] { left_--; round(); };
    bool ok = c_.write(w, w.index, text, [this, r, text, t0, next](uint8_t code) {
        if (code != wire::SUCCESS) { write.failed++; next(); return; }
        write.ms.push_back(now_() - t0);
        uint32_t t1 = now_();
        c_.read(r.root_pk, r.index, [this, r, text, t1, next](const ReadOutcome& o) {
            if (o.kind == ReadOutcome::FOUND && o.plaintext == text) hit.ms.push_back(now_() - t1);
            else hit.failed++;
            uint32_t t2 = now_();
            c_.read(r.root_pk, *r.index.next(), [this, t2, next](const ReadOutcome& o) {
                if (o.kind == ReadOutcome::NOT_FOUND) miss.ms.push_back(now_() - t2);
                else miss.failed++;
                next();
            });
        });
    });
    if (!ok) { write.failed++; left_ = 0; if (done_) done_(status()); }
}

}  // namespace kq
