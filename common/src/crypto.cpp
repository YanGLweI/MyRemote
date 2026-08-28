#include "crypto.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <cstring>

namespace crypto {

namespace {
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;
const char kHkdfInfo[] = "myremote/v1/session-key";
const uint8_t kHkdfSalt[] = {'M', 'y', 'R', 'e', 'm', 'o', 't', 'e'};
}  // namespace

Key derive_key(const std::string& psk) {
    Key out{};

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) {
        throw CryptoError("HKDF context creation failed");
    }

    size_t out_len = out.size();
    bool ok = EVP_PKEY_derive_init(ctx) == 1 &&
              EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1 &&
              EVP_PKEY_CTX_set1_hkdf_salt(ctx, kHkdfSalt, sizeof(kHkdfSalt)) == 1 &&
              EVP_PKEY_CTX_set1_hkdf_key(ctx, reinterpret_cast<const uint8_t*>(psk.data()),
                                         psk.size()) == 1 &&
              EVP_PKEY_CTX_add1_hkdf_info(ctx, reinterpret_cast<const uint8_t*>(kHkdfInfo),
                                          sizeof(kHkdfInfo) - 1) == 1 &&
              EVP_PKEY_derive(ctx, out.data(), &out_len) == 1 && out_len == out.size();

    EVP_PKEY_CTX_free(ctx);
    if (!ok) {
        throw CryptoError("HKDF key derivation failed");
    }
    return out;
}

std::vector<uint8_t> AesGcm::encrypt(const uint8_t* data, size_t len) {
    std::array<uint8_t, kNonceSize> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw CryptoError("Nonce generation failed");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw CryptoError("Cipher context creation failed");
    }

    std::vector<uint8_t> ciphertext(len + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;
    int final_len = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                  static_cast<int>(kNonceSize), nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce.data()) == 1 &&
              EVP_EncryptUpdate(ctx, ciphertext.data(), &out_len, data,
                                static_cast<int>(len)) == 1;

    size_t written = static_cast<size_t>(out_len);
    if (ok) {
        ok = EVP_EncryptFinal_ex(ctx, ciphertext.data() + written, &final_len) == 1;
        written += static_cast<size_t>(final_len);
    }

    std::array<uint8_t, kTagSize> tag{};
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagSize),
                                 tag.data()) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        throw CryptoError("AES-GCM encryption failed");
    }

    std::vector<uint8_t> result;
    result.reserve(kNonceSize + written + kTagSize);
    result.insert(result.end(), nonce.begin(), nonce.end());
    result.insert(result.end(), ciphertext.begin(), ciphertext.begin() + written);
    result.insert(result.end(), tag.begin(), tag.end());
    return result;
}

std::vector<uint8_t> AesGcm::decrypt(const uint8_t* data, size_t len) {
    if (len < kNonceSize + kTagSize) {
        throw CryptoError("Ciphertext too short");
    }

    const uint8_t* nonce = data;
    size_t ciphertext_len = len - kNonceSize - kTagSize;
    const uint8_t* ciphertext = data + kNonceSize;
    const uint8_t* tag = data + kNonceSize + ciphertext_len;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw CryptoError("Cipher context creation failed");
    }

    std::vector<uint8_t> plaintext(ciphertext_len + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;
    int final_len = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                  static_cast<int>(kNonceSize), nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce) == 1 &&
              EVP_DecryptUpdate(ctx, plaintext.data(), &out_len, ciphertext,
                                static_cast<int>(ciphertext_len)) == 1;

    size_t written = static_cast<size_t>(out_len);
    if (ok) {
        // Set expected tag; final fails on mismatch (tampering or wrong key).
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagSize),
                                 const_cast<uint8_t*>(tag)) == 1 &&
             EVP_DecryptFinal_ex(ctx, plaintext.data() + written, &final_len) == 1;
        written += static_cast<size_t>(final_len);
    }
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        throw CryptoError("AES-GCM authentication failed");
    }
    plaintext.resize(written);
    return plaintext;
}

std::vector<uint8_t> hmac_sha256(const std::string& key, const uint8_t* data, size_t len) {
    std::vector<uint8_t> out(32);
    unsigned int out_len = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data, len,
              out.data(), &out_len) || out_len != 32) {
        throw CryptoError("HMAC-SHA256 failed");
    }
    return out;
}

}  // namespace crypto
