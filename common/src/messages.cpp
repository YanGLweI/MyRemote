#include "messages.hpp"

#include <chrono>
#include <cstring>

namespace proto {

namespace {
constexpr size_t kDeviceIdFieldSize = 32;
constexpr size_t kDeviceNameFieldSize = 64;

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void put_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

void put_fixed_string(std::vector<uint8_t>& out, const std::string& value, size_t field_size) {
    size_t copy_len = value.size() < field_size ? value.size() : field_size;
    out.insert(out.end(), value.begin(), value.begin() + copy_len);
    out.insert(out.end(), field_size - copy_len, 0);
}

bool read_u16(const std::vector<uint8_t>& in, size_t& offset, uint16_t& out) {
    if (in.size() - offset < 2) return false;
    out = static_cast<uint16_t>((in[offset] << 8) | in[offset + 1]);
    offset += 2;
    return true;
}

bool read_u32(const std::vector<uint8_t>& in, size_t& offset, uint32_t& out) {
    if (in.size() - offset < 4) return false;
    out = (static_cast<uint32_t>(in[offset]) << 24) |
          (static_cast<uint32_t>(in[offset + 1]) << 16) |
          (static_cast<uint32_t>(in[offset + 2]) << 8) |
          static_cast<uint32_t>(in[offset + 3]);
    offset += 4;
    return true;
}

bool read_u64(const std::vector<uint8_t>& in, size_t& offset, uint64_t& out) {
    if (in.size() - offset < 8) return false;
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | in[offset + i];
    }
    offset += 8;
    return true;
}

std::string read_fixed_string(const std::vector<uint8_t>& in, size_t& offset, size_t field_size) {
    if (in.size() - offset < field_size) return {};
    const char* begin = reinterpret_cast<const char*>(in.data() + offset);
    size_t len = 0;
    while (len < field_size && begin[len] != '\0') ++len;
    offset += field_size;
    return std::string(begin, len);
}
}  // namespace

std::vector<uint8_t> make_register_payload(const std::string& device_id,
                                           const std::string& device_name,
                                           uint16_t screen_width,
                                           uint16_t screen_height) {
    std::vector<uint8_t> payload;
    payload.reserve(2 + kDeviceIdFieldSize + kDeviceNameFieldSize + 4);
    put_u16(payload, kProtocolVersion);
    put_fixed_string(payload, device_id, kDeviceIdFieldSize);
    put_fixed_string(payload, device_name, kDeviceNameFieldSize);
    put_u16(payload, screen_width);
    put_u16(payload, screen_height);
    return payload;
}

bool parse_register_payload(const std::vector<uint8_t>& payload, RegisterInfo& info) {
    size_t offset = 0;
    return read_u16(payload, offset, info.protocol_version) &&
           (info.device_id = read_fixed_string(payload, offset, kDeviceIdFieldSize), true) &&
           !info.device_id.empty() &&
           (info.device_name = read_fixed_string(payload, offset, kDeviceNameFieldSize), true) &&
           read_u16(payload, offset, info.screen_width) &&
           read_u16(payload, offset, info.screen_height);
}

std::vector<uint8_t> make_register_ack_payload(RegisterStatus status) {
    return {static_cast<uint8_t>(status)};
}

bool parse_register_ack_payload(const std::vector<uint8_t>& payload, RegisterStatus& status) {
    if (payload.empty()) return false;
    status = static_cast<RegisterStatus>(payload[0]);
    return true;
}

std::vector<uint8_t> make_heartbeat_payload() {
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    std::vector<uint8_t> payload;
    put_u64(payload, static_cast<uint64_t>(now));
    return payload;
}

std::vector<uint8_t> make_start_stream_payload(uint8_t fps, uint16_t bitrate_kbps) {
    std::vector<uint8_t> payload;
    payload.push_back(fps);
    put_u16(payload, bitrate_kbps);
    return payload;
}

bool parse_start_stream_payload(const std::vector<uint8_t>& payload, uint8_t& fps,
                                uint16_t& bitrate_kbps) {
    size_t offset = 0;
    if (payload.empty()) return false;
    fps = payload[0];
    offset = 1;
    return read_u16(payload, offset, bitrate_kbps);
}

std::vector<uint8_t> make_video_frame_payload(uint32_t seq, uint64_t timestamp_us,
                                              bool is_keyframe, const uint8_t* data,
                                              size_t len) {
    std::vector<uint8_t> payload;
    payload.reserve(13 + len);
    put_u32(payload, seq);
    put_u64(payload, timestamp_us);
    payload.push_back(is_keyframe ? 1 : 0);
    payload.insert(payload.end(), data, data + len);
    return payload;
}

bool parse_video_frame_payload(const std::vector<uint8_t>& payload, VideoFrameInfo& info) {
    size_t offset = 0;
    uint8_t keyframe_flag = 0;
    if (!read_u32(payload, offset, info.seq)) return false;
    if (!read_u64(payload, offset, info.timestamp_us)) return false;
    if (payload.size() - offset < 1) return false;
    keyframe_flag = payload[offset++];
    info.is_keyframe = keyframe_flag != 0;
    if (offset > payload.size()) return false;
    info.data = payload.data() + offset;
    info.size = payload.size() - offset;
    return true;
}

}  // namespace proto
