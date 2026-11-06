// SPDX-License-Identifier: AGPL-3.0-only
// The client around the engine: the actions it asks for are queued until a worker takes one,
// results come back as inputs, retries and deferred inputs wait for their time. And the
// Backend the executor runs on a device: boxes through the replica client, vouchers here.
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <map>

#include "kq1_engine.h"
#include "kq_replica.h"
#include "kq_view.h"

namespace kq {

class Runtime {
public:
    kq1::Engine eng;
    std::function<void(const kq1::Changes&)> on_changes;
    std::function<void(const kq1::FileWrite&)> on_file;  // a file the engine keeps, written before its changes
    std::function<void(kq1::Clip&&)> on_clip;             // a radio burst, to play; never written anywhere
    std::function<void(const kq1::Ping&)> on_ping;        // a radio ping, to sound
    // Engine steps taken so far: what a screen compares to know the state moved. Any task.
    std::atomic<uint32_t> steps{0};

    Runtime(kq1::State s, size_t max_plaintext) : eng(std::move(s), max_plaintext) {}

    void input(int64_t now_s, uint64_t now_ms, const kq1::Input& in);
    void resume(int64_t now_s);
    std::optional<kq1::Action> take();
    // The first ready action that ok accepts, in order.
    std::optional<kq1::Action> take_if(const std::function<bool(const kq1::Action&)>& ok);
    void finished(int64_t now_s, uint64_t now_ms, const kq1::Done& d);
    // Releases what has come due: retries still wanted, and deferred inputs.
    void tick(int64_t now_s, uint64_t now_ms);
    std::optional<uint64_t> next_due() const;
    bool busy() const { return !ready_.empty(); }

private:
    struct Later {
        uint64_t due;
        std::optional<kq1::Action> act;
        std::optional<kq1::Input> in;
    };
    void release(kq1::StepResult&& r);
    std::deque<kq1::Action> ready_;
    std::vector<Later> later_;
};

// Runs fn on the task that owns the replica client and waits until it calls done.
using Bridge = std::function<void(std::function<void(std::function<void()> done)> fn)>;

class ReplicaBackend : public kq1::Backend {
public:
    ReplicaBackend(ReplicaClient& c, Bridge bridge) : client_(c), bridge_(std::move(bridge)) {}

    std::optional<kq1::Keypair> new_keypair() override;
    Write write_box(const Bytes& write_cap, const kq1::Index& index, const Bytes& plaintext) override;
    std::optional<kq1::Read> read_box(const Bytes& read_cap, const kq1::Index& index) override;
    std::optional<size_t> max_plaintext() override { return client_.box_size(); }
    std::optional<kq1::Minted> voucher_mint(const Bytes& write_cap, const std::string& name, uint8_t form) override;
    std::optional<std::pair<Bytes, Bytes>> voucher_derive_stream(const Bytes& voucher) override;
    std::optional<kq1::Inducted> voucher_induct(const Bytes& voucher, const Bytes& payload, const Bytes& who_reply) override;
    std::optional<kq1::Opened> voucher_open(const Bytes& secret_key, const Bytes& sealed, const Bytes& write_cap) override;
    std::string error() const override { return error_; }

private:
    ReplicaClient& client_;
    Bridge bridge_;
    std::string error_ = "failed";
    // The seal public key of each secret key minted here, so opening a reply costs one CTIDH action, not two.
    std::map<Bytes, Bytes> seal_pub_;
};

// The chats as the screens show them: every one, or only `only`; with lines when `lines`.
std::vector<KqChat> chats(const kq1::State& st, const std::string& only, bool lines);
// A command's verb and arguments as the engine's input; nothing for an unknown verb.
std::optional<kq1::Input> input_of(const std::string& verb, const std::string& group, const std::string& arg);

}  // namespace kq
