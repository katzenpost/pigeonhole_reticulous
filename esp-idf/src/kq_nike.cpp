// SPDX-License-Identifier: AGPL-3.0-only
#include "kq_nike.h"

#include <cstring>

#include <sodium.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#elif !defined(ESP_PLATFORM)
#include <thread>
#endif

#define restrict __restrict  // the C headers use C99 restrict
extern "C" {
#include "csidh.h"
}
#undef restrict

namespace kq::nike {

static_assert(sizeof(private_key) == 130 && sizeof(public_key) == 128, "CTIDH-1024 sizes");

Shake::Shake(const uint8_t* seed, size_t len) {
    static_assert(sizeof(crypto_xof_shake256_state) <= sizeof st_, "shake state");
    auto* st = reinterpret_cast<crypto_xof_shake256_state*>(st_);
    crypto_xof_shake256_init(st);
    crypto_xof_shake256_update(st, seed, len);
}

void Shake::operator()(uint8_t* out, size_t n) {
    crypto_xof_shake256_squeeze(reinterpret_cast<crypto_xof_shake256_state*>(st_), out, n);
}

static void ctidh_rng(void* out, size_t n, uintptr_t ctx) { (*reinterpret_cast<const Rng*>(ctx))((uint8_t*)out, n); }

void in_turn(const Fn& a, const Fn& b) {
    a();
    b();
}

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
struct Job {
    const Fn* f;
    SemaphoreHandle_t done;
};

static void job_main(void* arg) {
    auto* j = (Job*)arg;
    SemaphoreHandle_t done = j->done;
    (*j->f)();
    xSemaphoreGive(done);
    vTaskDeleteWithCaps(nullptr);
}

void at_once(const Fn& a, const Fn& b) {
    // CTIDH-1024 peaks near 300 KB of stack, as for the firmware's kq.ctidh worker.
    constexpr uint32_t kStack = 384 * 1024;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    Job j{&b, done};
    if (!done || xTaskCreatePinnedToCoreWithCaps(job_main, "kq.ctidh2", kStack, &j, uxTaskPriorityGet(nullptr),
                                                 nullptr, !xPortGetCoreID(), MALLOC_CAP_SPIRAM) != pdPASS) {
        if (done) vSemaphoreDelete(done);
        return in_turn(a, b);
    }
    a();
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}
#elif defined(ESP_PLATFORM)
void at_once(const Fn& a, const Fn& b) { in_turn(a, b); }  // one core, or IDF's Linux target
#else
void at_once(const Fn& a, const Fn& b) {
    std::thread t(b);
    a();
    t.join();
}
#endif

void (*both)(const Fn&, const Fn&) = in_turn;

// A private key from rng: the X25519 key's 32 bytes, then the CTIDH key's.
static Bytes draw(const Rng& rng) {
    Bytes priv(kPrivateKeySize);
    rng(priv.data(), 32);
    private_key sk;
    csidh_private_withrng_bytes(&sk, (uintptr_t)&rng, ctidh_rng);
    memcpy(priv.data() + 32, &sk, sizeof sk);
    sodium_memzero(&sk, sizeof sk);
    return priv;
}

// The CTIDH action of priv's key on the curve in (128 bytes), into out; false when in is not a key.
static bool act(uint8_t* out, const uint8_t* priv, const void* in) {
    private_key sk;
    public_key pk, r;
    memcpy(&sk, priv + 32, sizeof sk);
    memcpy(&pk, in, sizeof pk);
    bool ok = csidh(&r, &pk, &sk);
    sodium_memzero(&sk, sizeof sk);
    if (ok) memcpy(out, &r, sizeof r);
    return ok;
}

static bool public_into(uint8_t* pub, const uint8_t* priv) {
    crypto_scalarmult_curve25519_base(pub, priv);
    return act(pub + 32, priv, &base);
}

Keypair generate(const Rng& rng) {
    Keypair k{Bytes(kPublicKeySize), draw(rng)};
    public_into(k.pub.data(), k.priv.data());  // the base curve is always valid
    return k;
}

Bytes public_of(const Bytes& priv) {
    if (priv.size() != kPrivateKeySize) return {};
    Bytes pub(kPublicKeySize);
    if (!public_into(pub.data(), priv.data())) return {};
    return pub;
}

bool valid_public(const Bytes& pub) {
    if (pub.size() != kPublicKeySize) return false;
    public_key pk;
    memcpy(&pk, pub.data() + 32, sizeof pk);
    return !sodium_is_zero(pub.data() + 32, sizeof pk) && validate(&pk);
}

// derive_secret; with own, priv's public key too, its CTIDH action beside the secret's by both.
static std::optional<Bytes> secret(const Bytes& priv, const Bytes& pub, Bytes* own) {
    if (priv.size() != kPrivateKeySize || pub.size() != kPublicKeySize) return std::nullopt;
    Bytes ss(kSecretSize);
    if (crypto_scalarmult_curve25519(ss.data(), priv.data(), pub.data()) != 0) return std::nullopt;
    bool ok = false;
    Fn shared = [&] {
        ok = !sodium_is_zero(pub.data() + 32, kPublicKeySize - 32) && act(ss.data() + 32, priv.data(), pub.data() + 32) &&
             !sodium_is_zero(ss.data() + 32, kSecretSize - 32);
    };
    if (own) {
        own->resize(kPublicKeySize);
        both(shared, [&] { public_into(own->data(), priv.data()); });
    } else {
        shared();
    }
    if (!ok) return std::nullopt;
    return ss;
}

std::optional<Bytes> derive_secret(const Bytes& priv, const Bytes& pub) { return secret(priv, pub, nullptr); }

// ---- sealed box ----

static constexpr char kSealLabel[] = "hpqc-nike-sealedbox-v1";

static void seal_keys(Bytes& ss, const uint8_t* eph_pub, const uint8_t* recipient_pub, const std::string& name,
                      uint8_t key[32], uint8_t nonce[24]) {
    Bytes info(kSealLabel, kSealLabel + sizeof kSealLabel - 1);
    info.push_back((uint8_t)(name.size() >> 8));
    info.push_back((uint8_t)name.size());
    info.insert(info.end(), name.begin(), name.end());
    info.insert(info.end(), eph_pub, eph_pub + kPublicKeySize);
    info.insert(info.end(), recipient_pub, recipient_pub + kPublicKeySize);
    uint8_t okm[56];
    hkdf_blake2b(32, ss.data(), ss.size(), nullptr, 0, info.data(), info.size(), okm, sizeof okm);
    memcpy(key, okm, 32);
    memcpy(nonce, okm + 32, 24);
    sodium_memzero(okm, sizeof okm);
    sodium_memzero(ss.data(), ss.size());
}

Bytes seal(const Bytes& recipient_pub, const uint8_t* pt, size_t len, const Rng& rng, const std::string& name) {
    Keypair eph{{}, draw(rng)};
    auto ss = secret(eph.priv, recipient_pub, &eph.pub);
    if (!ss) return {};
    uint8_t key[32], nonce[24];
    seal_keys(*ss, eph.pub.data(), recipient_pub.data(), name, key, nonce);
    Bytes out(eph.pub);
    out.resize(kPublicKeySize + crypto_secretbox_MACBYTES + len);
    crypto_secretbox_easy(out.data() + kPublicKeySize, pt, len, nonce, key);
    sodium_memzero(key, sizeof key);
    sodium_memzero(eph.priv.data(), eph.priv.size());
    return out;
}

std::optional<Bytes> open(const Bytes& priv, const Bytes& pub, const uint8_t* ct, size_t len, const std::string& name) {
    if (len < kPublicKeySize + crypto_secretbox_MACBYTES) return std::nullopt;
    Bytes eph(ct, ct + kPublicKeySize), own = pub;
    auto ss = secret(priv, eph, pub.empty() ? &own : nullptr);
    if (!ss) return std::nullopt;
    uint8_t key[32], nonce[24];
    seal_keys(*ss, eph.data(), own.data(), name, key, nonce);
    Bytes pt(len - kPublicKeySize - crypto_secretbox_MACBYTES);
    bool ok = crypto_secretbox_open_easy(pt.data(), ct + kPublicKeySize, len - kPublicKeySize, nonce, key) == 0;
    sodium_memzero(key, sizeof key);
    if (!ok) return std::nullopt;
    return pt;
}

// ---- MKEM ----

static std::optional<Bytes> mkem_secret(const Bytes& priv, const Bytes& pub, Bytes* own = nullptr) {
    auto raw = secret(priv, pub, own);
    if (!raw) return std::nullopt;
    Bytes s(32);
    crypto_generichash_blake2b(s.data(), 32, raw->data(), raw->size(), nullptr, 0);
    sodium_memzero(raw->data(), raw->size());
    return s;
}

static Bytes aead_seal(const uint8_t key[32], const uint8_t* pt, size_t len, const Rng& rng) {
    Bytes out(12 + len + 16);
    rng(out.data(), 12);
    crypto_aead_chacha20poly1305_ietf_encrypt(out.data() + 12, nullptr, pt, len, nullptr, 0, nullptr, out.data(), key);
    return out;
}

static std::optional<Bytes> aead_open(const uint8_t key[32], const Bytes& ct) {
    if (ct.size() < 12 + 16) return std::nullopt;
    const uint8_t* p = (const uint8_t*)ct.data();
    Bytes pt(ct.size() - 12 - 16);
    if (crypto_aead_chacha20poly1305_ietf_decrypt(pt.data(), nullptr, nullptr, p + 12, ct.size() - 12, nullptr, 0,
                                                  p, key) != 0)
        return std::nullopt;
    return pt;
}

Bytes mkem_encapsulate(const Bytes& recipient_pub, const uint8_t* pt, size_t len, const Rng& rng) {
    Keypair eph{{}, draw(rng)};
    auto secret = mkem_secret(eph.priv, recipient_pub, &eph.pub);
    sodium_memzero(eph.priv.data(), eph.priv.size());
    if (!secret) return {};
    uint8_t msg_key[32];
    rng(msg_key, 32);
    Bytes envelope = aead_seal(msg_key, pt, len, rng);
    Bytes dek = aead_seal(secret->data(), msg_key, 32, rng);
    sodium_memzero(msg_key, sizeof msg_key);
    sodium_memzero(secret->data(), secret->size());
    // Canonical CBOR orders the keys by length.
    Bytes out;
    kq1::push_header(out, 5, 3);
    kq1::push_text(out, "Envelope");
    kq1::push_bytes(out, envelope);
    kq1::push_text(out, "DEKCiphertexts");
    kq1::push_header(out, 4, 1);
    kq1::push_bytes(out, dek);
    kq1::push_text(out, "EphemeralPublicKey");
    kq1::push_bytes(out, eph.pub);
    return out;
}

std::optional<Bytes> mkem_decapsulate(const Bytes& priv, const uint8_t* ct, size_t len) {
    auto v = kq1::Term::read(ct, len);
    if (!v) return std::nullopt;
    auto* eph = v->field("EphemeralPublicKey");
    auto* deks = v->field("DEKCiphertexts");
    auto* env = v->field("Envelope");
    using T = kq1::Term;
    if (!eph || eph->kind != T::BYTES || !deks || deks->kind != T::LIST || !env || env->kind != T::BYTES)
        return std::nullopt;
    auto secret = mkem_secret(priv, eph->bytes);
    if (!secret) return std::nullopt;
    std::optional<Bytes> out;
    for (auto& d : deks->items) {
        if (d.kind != T::BYTES) continue;
        auto msg_key = aead_open(secret->data(), d.bytes);
        if (!msg_key || msg_key->size() != 32) continue;
        out = aead_open(msg_key->data(), env->bytes);
        sodium_memzero(msg_key->data(), msg_key->size());
        break;
    }
    sodium_memzero(secret->data(), secret->size());
    return out;
}

}  // namespace kq::nike
