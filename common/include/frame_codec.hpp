#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "messages.hpp"

namespace proto {

// Wire format: [4B big-endian length][1B type][payload]
// length covers type + payload (does not include the length field itself).
std::vector<uint8_t> encode_frame(MessageType type, const uint8_t* payload, size_t payload_len);

inline std::vector<uint8_t> encode_frame(MessageType type,
                                         const std::vector<uint8_t>& payload = {}) {
    return encode_frame(type, payload.data(), payload.size());
}

// Streaming reassembler for TCP byte streams (handles fragmentation and coalescing).
class FrameDecoder {
public:
    struct Frame {
        MessageType type;
        std::vector<uint8_t> payload;
    };

    // Append received bytes. Returns false on protocol violation (caller should disconnect).
    bool feed(const uint8_t* data, size_t len);

    bool has_frame() const { return !frames_.empty(); }
    Frame pop_frame();

private:
    bool parse_buffer();

    static constexpr size_t kLengthFieldSize = 4;
    static constexpr uint32_t kMaxPayloadSize = 64 * 1024 * 1024;

    std::vector<uint8_t> buffer_;
    std::vector<Frame> frames_;
};

}  // namespace proto
