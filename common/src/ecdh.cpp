#include "ecdh.hpp"
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/err.h>

void ECDHExchange::generate_keys() {
    // Create secp256r1 (P-256) elliptic curve context
    EC_KEY* ec_key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!ec_key) {
        throw std::runtime_error("Failed to create EC key");
    }
    
    // Generate private/public key pair
    if (EC_KEY_generate_key(ec_key) != 1) {
        EC_KEY_free(ec_key);
        throw std::runtime_error("Failed to generate EC keys");
    }
    
    ec_key_ = ec_key;
    
    // Create EVP_PKEY from EC_KEY
    private_key_ = EVP_PKEY_new();
    if (EVP_PKEY_assign_EC(private_key_, ec_key)) {
        ec_key_ = nullptr;  // Ownership transferred to EVP_PKEY
    } else {
        EVP_PKEY_free(private_key_);
        EC_KEY_free(ec_key);
        throw std::runtime_error("Failed to assign EC key to EVP_PKEY");
    }
}

std::vector<uint8_t> ECDHExchange::get_public_key() const {
    if (!ec_key_) {
        return {};
    }
    
    // Get public key in compressed format (33 bytes for P-256)
    int key_len = EC_get_pub_key_length(ec_key_);
    std::vector<uint8_t> public_key(key_len + 1);  // Add prefix byte
    
    unsigned char* ptr = public_key.data();
    EC_KEY_convert_to_convertible_format(ec_key_, &ptr, (unsigned long*)&public_key.size(),
                                         0, nullptr);  // Compressed format
    
    public_key.resize(public_key.size());
    return public_key;
}

bool ECDHExchange::set_peer_public_key(const std::vector<uint8_t>& peer_key) {
    if (!peer_key.empty()) {
        EC_KEY* peer_ec_key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        if (!peer_ec_key) {
            return false;
        }
        
        const BIGNUM* pub_x = nullptr;
        const BIGNUM* pub_y = nullptr;
        
        // Parse public key (assume uncompressed format: 0x04 + x(32) + y(32))
        BIGNUM* x = BN_bin2bn(peer_key.data() + 1, 32, nullptr);
        BIGNUM* y = BN_bin2bn(peer_key.data() + 33, 32, nullptr);
        
        if (x && y) {
            EC_KEY_set_public_key_affine_coordinates(peer_ec_key, x, y);
        }
        
        EC_KEY_free(peer_ec_key);
        BN_clear_free(x);
        BN_clear_free(y);
        
        return true;
    }
    
    return false;
}

std::array<uint8_t, 16> ECDHExchange::derive_shared_secret() const {
    if (!ec_key_ || !private_key_) {
        throw std::runtime_error("Keys not properly initialized");
    }
    
    // Derive shared secret using ECDH
    uint8_t* shared_secret = nullptr;
    size_t ss_len = 0;
    
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(private_key_, nullptr);
    if (!ctx) {
        throw std::runtime_error("Failed to create ECDH context");
    }
    
    if (EVP_PKEY_derive_init(ctx) <= 0 || 
        EVP_PKEY_derive_set_peer(ctx, peer_public_key_) <= 0 ||
        EVP_PKEY_derive(ctx, nullptr, &ss_len) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Failed to derive shared secret");
    }
    
    shared_secret = new uint8_t[ss_len];
    if (EVP_PKEY_derive(ctx, shared_secret, &ss_len) <= 0) {
        delete[] shared_secret;
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Shared secret derivation failed");
    }
    
    EVP_PKEY_CTX_free(ctx);
    
    // Hash the shared secret to get AES-128 key (use SHA256 and take first 16 bytes)
    std::array<uint8_t, 16> aes_key;
    unsigned int hash_len = 0;
    SHA256(shared_secret, ss_len, aes_key.data());
    
    delete[] shared_secret;
    return aes_key;
}
