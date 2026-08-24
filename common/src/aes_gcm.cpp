#include "aes_gcm.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cstring>

AESGCM::AESGCM(const std::array<uint8_t, 16>& key) : key_(key) {
    ctx_ = EVP_CIPHER_CTX_new();
    if (!ctx_) {
        throw EncryptionError("Failed to create cipher context");
    }
    
    if (EVP_CipherInit_ex(ctx_, EVP_aes_128_gcm(), nullptr, nullptr, nullptr, 
                          EVP_ENCRYPT) != 1) {
        throw EncryptionError("Failed to initialize AES-128-GCM");
    }
}

std::array<uint8_t, 12> AESGCM::generate_nonce() {
    std::array<uint8_t, 12> nonce;
    if (RAND_bytes(nonce.data(), nonce.size()) != 1) {
        throw EncryptionError("Failed to generate random nonce");
    }
    return nonce;
}

std::vector<uint8_t> AESGCM::encrypt(const std::vector<uint8_t>& plaintext) {
    // Generate fresh nonce for each encryption
    auto nonce = generate_nonce();
    
    // Set IV/nonce
    if (EVP_CipherUpdate(ctx_, nullptr, nullptr, nullptr, nonce.data()) != 1) {
        throw EncryptionError("Failed to set nonce");
    }
    
    // Calculate max encrypted size
    size_t max_size = plaintext.size() + EVP_MAX_BLOCK_LENGTH;
    std::vector<uint8_t> ciphertext(max_size);
    size_t out_len = 0;
    
    // Encrypt data
    if (EVP_CipherUpdate(ctx_, ciphertext.data(), &out_len, 
                         plaintext.data(), plaintext.size()) != 1) {
        throw EncryptionError("Encryption failed");
    }
    
    size_t encrypted_len = out_len;
    
    // Finalize encryption and get authentication tag
    uint8_t tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx_, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
        throw EncryptionError("Failed to get GCM tag");
    }
    
    // Build output: nonce[12] | ciphertext | mac[16]
    std::vector<uint8_t> result;
    result.reserve(12 + encrypted_len + 16);
    result.insert(result.end(), nonce.begin(), nonce.end());
    result.insert(result.end(), ciphertext.begin(), ciphertext.begin() + encrypted_len);
    result.insert(result.end(), tag, tag + 16);
    
    return result;
}

std::vector<uint8_t> AESGCM::decrypt(const std::vector<uint8_t>& encrypted_data) {
    if (encrypted_data.size() < 29) {  // 12 (nonce) + 1 (min ciphertext) + 16 (mac)
        throw EncryptionError("Encrypted data too short");
    }
    
    // Extract components
    auto nonce = std::array<uint8_t, 12>();
    std::copy(encrypted_data.begin(), encrypted_data.begin() + 12, nonce.begin());
    
    size_t ciphertext_len = encrypted_data.size() - 28;
    std::vector<uint8_t> tag(encrypted_data.end() - 16, encrypted_data.end());
    
    // Initialize decryption context with same key
    EVP_CIPHER_CTX* decrypt_ctx = EVP_CIPHER_CTX_new();
    EVP_CipherInit_ex(decrypt_ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr, EVP_DECRYPT);
    
    // Set nonce
    EVP_CipherUpdate(decrypt_ctx, nullptr, nullptr, nullptr, nonce.data());
    
    // Setup authentication tag
    EVP_CIPHER_CTX_ctrl(decrypt_ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data());
    
    // Decrypt
    std::vector<uint8_t> plaintext(ciphertext_len + EVP_MAX_BLOCK_LENGTH);
    size_t out_len = 0;
    
    if (EVP_CipherUpdate(decrypt_ctx, plaintext.data(), &out_len,
                         encrypted_data.data() + 12, ciphertext_len) != 1) {
        EVP_CIPHER_CTX_free(decrypt_ctx);
        throw EncryptionError("Decryption failed");
    }
    
    // Verify tag
    if (EVP_CipherFinal_ex(decrypt_ctx, plaintext.data() + out_len, &out_len) != 1) {
        EVP_CIPHER_CTX_free(decrypt_ctx);
        throw EncryptionError("MAC verification failed - tampering detected");
    }
    
    EVP_CIPHER_CTX_free(decrypt_ctx);
    plaintext.resize(out_len);
    return plaintext;
}

SecureChannel::SecureChannel(const std::array<uint8_t, 16>& key)
    : encryptor_(key), decryptor_(key) {}

void SecureChannel::send(std::vector<uint8_t>& buffer, const std::vector<uint8_t>& plaintext) {
    buffer = encryptor_.encrypt(plaintext);
}

bool SecureChannel::receive(std::vector<uint8_t>& plaintext, std::vector<uint8_t>& encrypted_buffer) {
    try {
        plaintext = decryptor_.decrypt(encrypted_buffer);
        return true;
    } catch (const EncryptionError& e) {
        return false;
    }
}
