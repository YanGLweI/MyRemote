#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace crypto {

class CryptoError : public std::runtime_error {
public:
    explicit CryptoError(const std::string& msg) : std::runtime_error(msg) {}
};

using Key = std::array<uint8_t, 16>;

// Derive an AES-128 key from the pre-shared connection secret (HKDF-SHA256).
Key derive_key(const std::string& psk);

// AES-128-GCM with per-message random nonce.
// Output layout: nonce[12] | ciphertext | tag[16].
class AesGcm {
public:
    explicit AesGcm(const Key& key) : key_(key) {}

    std::vector<uint8_t> encrypt(const uint8_t* data, size_t len);
    std::vector<uint8_t> encrypt(const std::vector<uint8_t>& data) {
        return encrypt(data.data(), data.size());
    }

    // Throws CryptoError when authentication fails (tampering or wrong key).
    std::vector<uint8_t> decrypt(const uint8_t* data, size_t len);
    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data) {
        return decrypt(data.data(), data.size());
    }

private:
    Key key_;
};

}  // namespace crypto
