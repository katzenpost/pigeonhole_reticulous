// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_vault.h"

#include <sodium.h>

#include <chrono>

#include "kq1_trace.h"

namespace kq {

using kq1::trace::Bad;

namespace {

constexpr size_t kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, kTag = crypto_aead_xchacha20poly1305_ietf_ABYTES;
const std::string kLockAd = "kq lock 1";

Bytes seal(const Bytes& key, const std::string& ad, const uint8_t* p, size_t n) {
    Bytes out(kNonce + n + kTag);
    randombytes_buf(out.data(), kNonce);
    unsigned long long len = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.data() + kNonce, &len, p, n, (const uint8_t*)ad.data(), ad.size(),
                                               nullptr, out.data(), key.data());
    return out;
}

std::optional<std::string> open(const Bytes& key, const std::string& ad, const uint8_t* p, size_t n) {
    if (n < kNonce + kTag) return std::nullopt;
    std::string out(n - kNonce - kTag, '\0');
    unsigned long long len = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt((uint8_t*)out.data(), &len, nullptr, p + kNonce, n - kNonce,
                                                   (const uint8_t*)ad.data(), ad.size(), p, key.data()) != 0)
        return std::nullopt;
    return out;
}

std::string hex(const Bytes& b) { return kq1::trace::h(b).s; }

// Each piece: its length (u32, big endian), then the piece sealed with the file's name as
// associated data, so a piece cannot be moved to another file.
std::string piece(const Bytes& key, const std::string& name, const std::string& plain) {
    Bytes s = seal(key, name, (const uint8_t*)plain.data(), plain.size());
    std::string out(4, '\0');
    for (int i = 0; i < 4; i++) out[i] = (char)(s.size() >> (24 - 8 * i));
    out.append(s.begin(), s.end());
    return out;
}

}  // namespace

StoreFiles sealed_files(StoreFiles inner, const Bytes& key) {
    StoreFiles f;
    f.read = [inner, key](const std::string& name) -> std::optional<std::string> {
        auto kept = inner.read(name + ".s");
        if (!kept) return std::nullopt;
        std::string out;
        const uint8_t* p = (const uint8_t*)kept->data();
        size_t n = kept->size(), at = 0;
        while (at + 4 <= n) {
            size_t len = (size_t)p[at] << 24 | (size_t)p[at + 1] << 16 | (size_t)p[at + 2] << 8 | p[at + 3];
            if (at + 4 + len > n) break;  // the last piece, cut short as it was written
            auto plain = open(key, name, p + at + 4, len);
            if (!plain) throw Bad(name + ": does not open (another key, or changed)");
            out += *plain;
            sodium_memzero(plain->data(), plain->size());
            at += 4 + len;
        }
        return out;
    };
    f.replace = [inner, key](const std::string& name, const std::string& d) { return inner.replace(name + ".s", piece(key, name, d)); };
    f.append = [inner, key](const std::string& name, const std::string& d) { return inner.append(name + ".s", piece(key, name, d)); };
    f.remove = [inner](const std::string& name) { inner.remove(name + ".s"); };
    return f;
}

Vault::Vault(StoreFiles files) : inner_(std::move(files)) {
    auto lock = inner_.read("lock.json");
    if (!lock) {
        if (inner_.read("state.json.s"))
            set(KqLock::LOST, "the chats are sealed but lock.json, which keeps their key, is gone");
        else
            set(KqLock::UNDECIDED, inner_.read("state.json") ? "Chats you have from before are kept unencrypted; a passphrase seals them." : "");
        return;
    }
    try {
        kq::Json j = kq::Json::parse(*lock);
        set(kq1::trace::bo(kq1::trace::get(j, "sealed")) ? KqLock::LOCKED : KqLock::OPEN, "");
    } catch (const std::exception& e) {
        set(KqLock::LOST, std::string("lock.json cannot be read: ") + e.what());
    }
}

void Vault::set(KqLock k, const std::string& why) {
    std::lock_guard<std::mutex> g(mu_);
    state_ = k;
    why_ = why;
}

KqLock Vault::state(std::string* why) const {
    std::lock_guard<std::mutex> g(mu_);
    if (why) *why = why_;
    return state_;
}

void Vault::ask_setup(std::string passphrase) {
    std::lock_guard<std::mutex> g(mu_);
    if (state_ != KqLock::UNDECIDED) return;
    asked_ = {false, std::move(passphrase)};
    state_ = KqLock::UNLOCKING;
}

void Vault::ask_unlock(std::string passphrase) {
    std::lock_guard<std::mutex> g(mu_);
    if (state_ != KqLock::LOCKED) return;
    asked_ = {true, std::move(passphrase)};
    state_ = KqLock::UNLOCKING;
}

bool Vault::serve() {
    std::optional<std::pair<bool, std::string>> a;
    {
        std::lock_guard<std::mutex> g(mu_);
        a.swap(asked_);
    }
    if (a) {
        bool ok = a->first ? unlock(a->second) : setup(a->second);
        sodium_memzero(a->second.data(), a->second.size());
        if (!ok) return false;
    }
    if (state(nullptr) != KqLock::OPEN || told_) return false;
    told_ = true;
    return true;
}

bool Vault::derive(Bytes& kek, const std::string& pass, const Bytes& salt, uint64_t o, size_t m) {
    auto t0 = std::chrono::steady_clock::now();
    boost(true);
    int r = crypto_pwhash(kek.data(), kek.size(), pass.data(), pass.size(), salt.data(), o, m, crypto_pwhash_ALG_ARGON2ID13);
    boost(false);
    last_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    last_mem = m;
    last_ops = o;
    return r == 0;
}

static std::string kib(size_t n) { return std::to_string(n >> 10) + " KiB"; }

StoreFiles Vault::files() const { return key_ ? sealed_files(inner_, *key_) : inner_; }

bool Vault::setup(const std::string& pass) {
    if (pass.empty()) {
        if (!inner_.replace("lock.json", kq::Json::obj({{"version", kq::Json::num(1)}, {"sealed", kq::Json::boolean(false)}}).dump())) {
            set(KqLock::UNDECIDED, "lock.json could not be written");
            return false;
        }
        set(KqLock::OPEN, "");
        return true;
    }
    // As much memory as is free, up to mem, leaving spare for everything else; the passes make
    // up for less of it.
    size_t free = room(), m = mem;
    while (m > least && m + spare > free) m /= 2;
    if (m + spare > free) {
        set(KqLock::UNDECIDED, "not enough memory free right now (" + kib(free) + " in one piece, " +
                                   kib(least + spare) + " needed): try again, or choose No passphrase");
        return false;
    }
    uint64_t o = (ops * mem + m - 1) / m;
    Bytes key(32), salt(crypto_pwhash_SALTBYTES), kek(32);
    randombytes_buf(key.data(), key.size());
    randombytes_buf(salt.data(), salt.size());
    if (!derive(kek, pass, salt, o, m)) {
        set(KqLock::UNDECIDED, "not enough memory to derive the key");
        return false;
    }
    Bytes wrapped = seal(kek, kLockAd, key.data(), key.size());
    sodium_memzero(kek.data(), kek.size());
    // Chats kept unencrypted before are sealed first; lock.json, written last, is what makes the
    // sealed copy the one that counts, and then the unencrypted one goes.
    Store plain(inner_);
    kq1::State old = plain.load("");
    bool had = inner_.read("state.json").has_value();
    if (had && !Store(sealed_files(inner_, key)).save_all(old)) {
        set(KqLock::UNDECIDED, "the chats could not be sealed");
        return false;
    }
    kq::Json lock = kq::Json::obj({{"version", kq::Json::num(1)},
                                   {"sealed", kq::Json::boolean(true)},
                                   {"kdf", kq::Json::str("argon2id13")},
                                   {"ops", kq::Json::num(o)},
                                   {"mem", kq::Json::num(m)},
                                   {"salt", kq::Json::str(hex(salt))},
                                   {"wrapped", kq::Json::str(hex(wrapped))}});
    if (!inner_.replace("lock.json", lock.dump())) {
        set(KqLock::UNDECIDED, "lock.json could not be written");
        return false;
    }
    if (had) plain.erase(old);
    key_ = key;
    sodium_memzero(key.data(), key.size());
    set(KqLock::OPEN, "");
    return true;
}

bool Vault::unlock(const std::string& pass) {
    Bytes kek(32);
    std::optional<std::string> key;
    try {
        kq::Json j = kq::Json::parse(inner_.read("lock.json").value_or(""));
        using namespace kq1::trace;
        if (get(j, "kdf").s != "argon2id13") throw Bad("unknown kdf");
        Bytes salt = b(get(j, "salt")), wrapped = b(get(j, "wrapped"));
        if (salt.size() != crypto_pwhash_SALTBYTES) throw Bad("salt");
        uint64_t o = u(get(j, "ops"));
        size_t m = (size_t)u(get(j, "mem")), free = room();
        if (m + spare > free) {
            set(KqLock::LOCKED, "not enough memory free right now to unlock (" + kib(free) + " in one piece, " +
                                    kib(m + spare) + " needed): try again");
            return false;
        }
        if (!derive(kek, pass, salt, o, m)) {
            set(KqLock::LOCKED, "not enough memory to derive the key: try again");
            return false;
        }
        key = open(kek, kLockAd, wrapped.data(), wrapped.size());
    } catch (const std::exception& e) {
        set(KqLock::LOST, std::string("lock.json cannot be read: ") + e.what());
        return false;
    }
    sodium_memzero(kek.data(), kek.size());
    if (!key || key->size() != 32) {
        set(KqLock::LOCKED, "That is not the passphrase.");
        return false;
    }
    key_ = Bytes(key->begin(), key->end());
    sodium_memzero(key->data(), key->size());
    // A sealing cut short after lock.json was written left the unencrypted chats behind.
    if (inner_.read("state.json")) {
        Store plain(inner_);
        plain.erase(plain.load(""));
    }
    set(KqLock::OPEN, "");
    return true;
}

}  // namespace kq
