// SPDX-License-Identifier: AGPL-3.0-only
// The chats at rest, sealed or not, as the user chose at first start. Sealed, every file is
// sealed whole under a random key of 32 bytes; that key is kept in lock.json wrapped under a
// key derived from the passphrase with Argon2id, so it is the passphrase that unlocks, and
// nothing on the device can without it. A state already sealed is never given a new key.
#pragma once
#include <functional>
#include <mutex>
#include <optional>

#include "kq_crypto.h"
#include "kq_store.h"
#include "kq_view.h"

namespace kq {

// The files under `inner`, each sealed whole or in appended pieces, named with ".s" after.
StoreFiles sealed_files(StoreFiles inner, const Bytes& key);

class Vault {
public:
    // Argon2id's cost, kept in lock.json with the salt: `mem` when that much is free, else less
    // memory and more passes, as long as ops times mem stays at `work`.
    static inline uint64_t ops = 3;
    static inline size_t mem = 4 << 20, least = 1 << 20, spare = 512 << 10;
    // The platform's: the largest block that can be allocated now, and the CPU held at full
    // speed (true) and let go (false) around the derivation.
    static inline std::function<size_t()> room = [] { return (size_t)-1; };
    static inline std::function<void(bool)> boost = [](bool) {};
    // How long the last derivation took, and with what.
    uint32_t last_ms = 0;
    size_t last_mem = 0;
    uint64_t last_ops = 0;

    explicit Vault(StoreFiles files);
    // Any task.
    KqLock state(std::string* why = nullptr) const;
    void ask_setup(std::string passphrase);
    void ask_unlock(std::string passphrase);
    // On the task that owns the store: runs what was asked; true once, when the chats can be
    // read, after which files() are the ones to keep them in.
    bool serve();
    StoreFiles files() const;

private:
    bool setup(const std::string& pass);
    bool unlock(const std::string& pass);
    void set(KqLock k, const std::string& why);
    bool derive(Bytes& kek, const std::string& pass, const Bytes& salt, uint64_t ops, size_t mem);

    StoreFiles inner_;
    std::optional<Bytes> key_;
    bool told_ = false;
    mutable std::mutex mu_;
    KqLock state_ = KqLock::STARTING;
    std::string why_;
    std::optional<std::pair<bool, std::string>> asked_;  // unlock?, passphrase
};

}  // namespace kq
