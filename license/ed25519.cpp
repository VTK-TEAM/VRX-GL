#include "license/ed25519.hpp"

#include <openssl/evp.h>

namespace vrx::license {

bool ed25519_verify(const uint8_t* msg, size_t msg_len,
                    const uint8_t* sig, const uint8_t* pub) {
    EVP_PKEY* pk =
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub, 32);
    if (!pk) return false;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { EVP_PKEY_free(pk); return false; }

    bool ok = false;
    // Ed25519 — одноразова верифікація (EVP_DigestVerify), не потоковий
    // Update/Final: сам алгоритм гешує все повідомлення всередині.
    if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pk) == 1) {
        ok = (EVP_DigestVerify(ctx, sig, 64, msg, msg_len) == 1);
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return ok;
}

} // namespace vrx::license
