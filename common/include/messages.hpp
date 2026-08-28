#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace proto {

constexpr uint16_t kProtocolVersion = 1;

enum class MessageType : uint8_t {
    Register        = 0x01,  // C→S device registration
    RegisterAck     = 0x02,  // S→C accept/reject
    Heartbeat       = 0x03,  // C→S keep-alive
    StartStream     = 0x04,  // S→C start desktop streaming (with quality params)
    StopStream      = 0x05,  // S→C stop streaming
    VideoFrame      = 0x06,  // C→S encoded frame
    InputEvent      = 0x07,  // S→C mouse/keyboard event
    RequestKeyframe = 0x08,  // S→C force I-frame
    AuthChallenge   = 0x09,  // S→C secondary password challenge
    AuthResponse    = 0x0A,  // C→S secondary password response
    Encrypted       = 0x10,  // envelope: AES-GCM over [inner type][payload]
};

enum class RegisterStatus : uint8_t {
    Ok = 0,
    Rejected = 1,
    ServerFull = 2,
};

// Capability bits carried in the trailing Register flags byte.
constexpr uint8_t kRegisterFlagElevated = 0x01;

struct RegisterInfo {
    uint16_t protocol_version = 0;
    std::string device_id;
    std::string device_name;
    uint16_t screen_width = 0;
    uint16_t screen_height = 0;
    bool elevated = false;
    // False for agents too old to send the flags byte: elevation stays unknown.
    bool elevation_known = false;
};

enum class InputKind : uint8_t {
    MouseMove = 1,   // [2B x][2B y] in remote pixels
    MouseButton = 2, // [1B button][1B pressed]
    MouseWheel = 3,  // [2B delta]
    Key = 4,         // [2B vk][1B pressed][1B extended]
};

std::vector<uint8_t> make_mouse_move(uint16_t x, uint16_t y);
std::vector<uint8_t> make_mouse_button(uint8_t button, bool pressed);
std::vector<uint8_t> make_mouse_wheel(int16_t delta);
std::vector<uint8_t> make_key(uint16_t vk, bool pressed, bool extended);

struct InputEvent {
    InputKind kind;
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t button = 0;
    uint16_t vk = 0;
    int16_t delta = 0;
    bool pressed = false;
    bool extended = false;
};
bool parse_input_event(const std::vector<uint8_t>& payload, InputEvent& out);

struct VideoFrameInfo {
    uint32_t seq = 0;
    uint64_t timestamp_us = 0;
    bool is_keyframe = false;
    const uint8_t* data = nullptr;  // points into the payload buffer
    size_t size = 0;
};

// Payload builders / parsers (all multi-byte integers are big-endian).
std::vector<uint8_t> make_register_payload(const std::string& device_id,
                                           const std::string& device_name,
                                           uint16_t screen_width, uint16_t screen_height,
                                           std::optional<uint8_t> flags = std::nullopt);
bool parse_register_payload(const std::vector<uint8_t>& payload, RegisterInfo& info);

std::vector<uint8_t> make_register_ack_payload(RegisterStatus status);
bool parse_register_ack_payload(const std::vector<uint8_t>& payload, RegisterStatus& status);

std::vector<uint8_t> make_heartbeat_payload();

std::vector<uint8_t> make_start_stream_payload(uint8_t fps, uint16_t bitrate_kbps,
                                               uint16_t max_encode_width = 0);
bool parse_start_stream_payload(const std::vector<uint8_t>& payload, uint8_t& fps,
                                uint16_t& bitrate_kbps,
                                uint16_t& max_encode_width);

std::vector<uint8_t> make_video_frame_payload(uint32_t seq, uint64_t timestamp_us,
                                              bool is_keyframe, const uint8_t* data,
                                              size_t len);
bool parse_video_frame_payload(const std::vector<uint8_t>& payload, VideoFrameInfo& info);

}  // namespace proto
