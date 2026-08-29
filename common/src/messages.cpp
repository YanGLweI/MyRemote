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
                                           uint16_t screen_height,
                                           std::optional<uint8_t> flags) {
    std::vector<uint8_t> payload;
    payload.reserve(2 + kDeviceIdFieldSize + kDeviceNameFieldSize + 5);
    put_u16(payload, kProtocolVersion);
    put_fixed_string(payload, device_id, kDeviceIdFieldSize);
    put_fixed_string(payload, device_name, kDeviceNameFieldSize);
    put_u16(payload, screen_width);
    put_u16(payload, screen_height);
    if (flags) {
        payload.push_back(*flags);
    }
    return payload;
}

bool parse_register_payload(const std::vector<uint8_t>& payload, RegisterInfo& info) {
    size_t offset = 0;
    bool ok = read_u16(payload, offset, info.protocol_version) &&
           (info.device_id = read_fixed_string(payload, offset, kDeviceIdFieldSize), true) &&
           !info.device_id.empty() &&
           (info.device_name = read_fixed_string(payload, offset, kDeviceNameFieldSize), true) &&
           read_u16(payload, offset, info.screen_width) &&
           read_u16(payload, offset, info.screen_height);
    if (!ok) return false;
    info.elevation_known = offset < payload.size();
    if (info.elevation_known) {
        const uint8_t flags = payload[offset];
        info.flags = flags;
        info.elevated = (flags & kRegisterFlagElevated) != 0;
        info.service_host = (flags & kFlagServiceHost) != 0;
        info.is_system = (flags & kFlagIsSystem) != 0;
    }
    return true;
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

std::vector<uint8_t> make_start_stream_payload(uint8_t fps, uint16_t bitrate_kbps,
                                               uint16_t max_encode_width) {
    std::vector<uint8_t> payload;
    payload.push_back(fps);
    put_u16(payload, bitrate_kbps);
    put_u16(payload, max_encode_width);
    return payload;
}

bool parse_start_stream_payload(const std::vector<uint8_t>& payload, uint8_t& fps,
                                uint16_t& bitrate_kbps,
                                uint16_t& max_encode_width) {
    size_t offset = 0;
    if (payload.empty()) return false;
    fps = payload[0];
    offset = 1;
    max_encode_width = 0;
    if (!read_u16(payload, offset, bitrate_kbps)) return false;
    read_u16(payload, offset, max_encode_width);  // optional: 0 = device default
    return true;
}

std::vector<uint8_t> make_display_changed_payload(uint16_t width, uint16_t height) {
    std::vector<uint8_t> payload;
    put_u16(payload, width);
    put_u16(payload, height);
    return payload;
}

bool parse_display_changed_payload(const std::vector<uint8_t>& payload,
                                   uint16_t& width, uint16_t& height) {
    size_t offset = 0;
    return read_u16(payload, offset, width) &&
           read_u16(payload, offset, height);
}

std::vector<uint8_t> make_state_report_payload(uint8_t flags, uint16_t width,
                                               uint16_t height) {
    std::vector<uint8_t> payload;
    payload.push_back(flags);
    put_u16(payload, width);
    put_u16(payload, height);
    return payload;
}

bool parse_state_report_payload(const std::vector<uint8_t>& payload, uint8_t& flags,
                                uint16_t& width, uint16_t& height) {
    if (payload.size() < 5) return false;
    size_t offset = 0;
    flags = payload[offset++];
    return read_u16(payload, offset, width) && read_u16(payload, offset, height);
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

namespace {
void put_u8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
}

std::vector<uint8_t> make_mouse_move(uint16_t x, uint16_t y) {
    std::vector<uint8_t> p;
    put_u8(p, static_cast<uint8_t>(InputKind::MouseMove));
    put_u16(p, x);
    put_u16(p, y);
    return p;
}

std::vector<uint8_t> make_mouse_button(uint8_t button, bool pressed) {
    std::vector<uint8_t> p;
    put_u8(p, static_cast<uint8_t>(InputKind::MouseButton));
    put_u8(p, button);
    put_u8(p, pressed ? 1 : 0);
    return p;
}

std::vector<uint8_t> make_mouse_wheel(int16_t delta) {
    std::vector<uint8_t> p;
    put_u8(p, static_cast<uint8_t>(InputKind::MouseWheel));
    put_u16(p, static_cast<uint16_t>(delta));
    return p;
}

std::vector<uint8_t> make_key(uint16_t vk, bool pressed, bool extended) {
    std::vector<uint8_t> p;
    put_u8(p, static_cast<uint8_t>(InputKind::Key));
    put_u16(p, vk);
    put_u8(p, pressed ? 1 : 0);
    put_u8(p, extended ? 1 : 0);
    return p;
}

bool parse_input_event(const std::vector<uint8_t>& payload, InputEvent& out) {
    if (payload.empty()) return false;
    size_t off = 0;
    out = InputEvent{};
    out.kind = static_cast<InputKind>(payload[off++]);
    switch (out.kind) {
        case InputKind::MouseMove:
            return read_u16(payload, off, out.x) && read_u16(payload, off, out.y);
        case InputKind::MouseButton: {
            uint8_t pressed = 0;
            if (off + 2 > payload.size()) return false;
            out.button = payload[off++];
            pressed = payload[off++];
            out.pressed = pressed != 0;
            return true;
        }
        case InputKind::MouseWheel: {
            uint16_t d = 0;
            if (!read_u16(payload, off, d)) return false;
            out.delta = static_cast<int16_t>(d);
            return true;
        }
        case InputKind::Key: {
            uint8_t pressed = 0, extended = 0;
            if (!read_u16(payload, off, out.vk)) return false;
            if (off + 2 > payload.size()) return false;
            pressed = payload[off++];
            extended = payload[off++];
            out.pressed = pressed != 0;
            out.extended = extended != 0;
            return true;
        }
        default:
            return false;
    }
}


}  // namespace proto
