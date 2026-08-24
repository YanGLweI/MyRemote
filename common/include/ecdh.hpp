#pragma once

#include <array>
#include <vector>

// ECDH key exchange for session key derivation
// Used to establish unique encryption keys per connection
class ECDHExchange {
public:
    // Generate private/public key pair
    void generate_keys();
    
    // Get public key bytes (65 compressed format)
    std::vector<uint8_t> get_public_key() const;
    
    // Set peer's public key from byte array
    bool set_peer_public_key(const std::vector<uint8_t>& peer_key);
    
    // Derive shared secret and generate AES-128 session key
    std::array<uint8_t, 16> derive_shared_secret() const;
    
private:
    EVP_PKEY* private_key_ = nullptr;
    EVP_PKEY* peer_public_key_ = nullptr;
    EC_KEY* ec_key_ = nullptr;
};
