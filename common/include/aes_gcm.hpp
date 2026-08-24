#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <stdexcept>

// AES-128-GCM encryption module
// Lightweight application-layer encryption without TLS dependency
class AESGCM {
public:
    // Initialize with a 128-bit key (32 hex characters)
    explicit AESGCM(const std::array<uint8_t, 16>& key);
    
    // Encrypt plaintext data
    // Returns: encrypted message containing {nonce[12] | ciphertext | mac[16]}
    std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext);
    
    // Decrypt encrypted message
    // Throws exception if MAC verification fails
    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& encrypted_data);
    
private:
    EVP_CIPHER_CTX* ctx_;
    const std::array<uint8_t, 16>& key_;
    
    // Generate random nonce (12 bytes for GCM)
    std::array<uint8_t, 12> generate_nonce();
};

// Convenience class for bidirectional communication
class SecureChannel {
public:
    SecureChannel(const std::array<uint8_t, 16>& key);
    
    void send(std::vector<uint8_t>& buffer, const std::vector<uint8_t>& plaintext);
    bool receive(std::vector<uint8_t>& plaintext, std::vector<uint8_t>& encrypted_buffer);
    
private:
    AESGCM encryptor_;
    AESGCM decryptor_;
};

// Custom exceptions
class EncryptionError : public std::runtime_error {
public:
    explicit EncryptionError(const std::string& msg) 
        : runtime_error(msg) {}
};
