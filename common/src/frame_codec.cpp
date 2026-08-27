#include "frame_codec.hpp"

#include <cstring>

namespace proto {

std::vector<uint8_t> encode_frame(MessageType type, const uint8_t* payload, size_t payload_len) {
    std::vector<uint8_t> frame(4 + 1 + payload_len);
    uint32_t length = static_cast<uint32_t>(1 + payload_len);
    frame[0] = static_cast<uint8_t>((length >> 24) & 0xFF);
    frame[1] = static_cast<uint8_t>((length >> 16) & 0xFF);
    frame[2] = static_cast<uint8_t>((length >> 8) & 0xFF);
    frame[3] = static_cast<uint8_t>(length & 0xFF);
    frame[4] = static_cast<uint8_t>(type);
    if (payload_len > 0) {
        std::memcpy(frame.data() + 5, payload, payload_len);
    }
    return frame;
}


bool FrameDecoder::feed(const uint8_t* data, size_t len) {
    buffer_.insert(buffer_.end(), data, data + len);
    return parse_buffer();
}

FrameDecoder::Frame FrameDecoder::pop_frame() {
    Frame frame = std::move(frames_.front());
    frames_.erase(frames_.begin());
    return frame;
}

bool FrameDecoder::parse_buffer() {
    size_t offset = 0;
    while (buffer_.size() - offset >= kLengthFieldSize) {
        uint32_t length = (static_cast<uint32_t>(buffer_[offset]) << 24) |
                          (static_cast<uint32_t>(buffer_[offset + 1]) << 16) |
                          (static_cast<uint32_t>(buffer_[offset + 2]) << 8) |
                          static_cast<uint32_t>(buffer_[offset + 3]);
        if (length < 1 || length - 1 > kMaxPayloadSize) {
            return false;  // protocol violation
        }
        size_t frame_total = kLengthFieldSize + length;
        if (buffer_.size() - offset < frame_total) {
            break;  // incomplete frame, wait for more data
        }
        Frame frame;
        frame.type = static_cast<MessageType>(buffer_[offset + 4]);
        frame.payload.assign(buffer_.begin() + offset + 5,
                             buffer_.begin() + offset + frame_total);
        frames_.push_back(std::move(frame));
        offset += frame_total;
    }
    if (offset > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + offset);
    }
    return true;
}

}  // namespace proto
