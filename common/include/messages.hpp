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
    DisplayChanged  = 0x0B,  // C→S native desktop size changed [2B w][2B h]
    AttachConsole   = 0x0C,  // S→C reattach this session to the physical console
    StateReport     = 0x0D,  // C→S live capability + geometry [1B flags][2B w][2B h]
    LockWorkstation = 0x0F,  // S→C hand the machine back to its logon screen
    Encrypted       = 0x10,  // envelope: AES-GCM over [inner type][payload]
};

enum class RegisterStatus : uint8_t {
    Ok = 0,
    Rejected = 1,
    ServerFull = 2,
};

// Capability bits carried in the trailing Register flags byte, reused verbatim
// by StateReport so a live change never needs a second registration.
constexpr uint8_t kRegisterFlagElevated = 0x01;
constexpr uint8_t kFlagServiceHost = 0x02;       // started by the MyRemote service
constexpr uint8_t kFlagIsSystem = 0x04;          // LocalSystem (implies Elevated)
constexpr uint8_t kFlagConsoleOwner = 0x08;      // this session owns the console
constexpr uint8_t kFlagSecureDesktop = 0x10;     // can follow Winlogon (SeTcb)
constexpr uint8_t kFlagLogonScreen = 0x20;       // input desktop is not "Default"

struct RegisterInfo {
    uint16_t protocol_version = 0;
    std::string device_id;
    std::string device_name;
    uint16_t screen_width = 0;
    uint16_t screen_height = 0;
    bool elevated = false;
    // False for agents too old to send the flags byte: elevation stays unknown.
    bool elevation_known = false;
    uint8_t flags = 0;
    bool service_host = false;
    bool is_system = false;
    bool console_owner = false;
    bool secure_desktop = false;
    bool logon_screen = false;
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

std::vector<uint8_t> make_display_changed_payload(uint16_t width, uint16_t height);
bool parse_display_changed_payload(const std::vector<uint8_t>& payload,
                                   uint16_t& width, uint16_t& height);

std::vector<uint8_t> make_state_report_payload(uint8_t flags, uint16_t width,
                                               uint16_t height);
bool parse_state_report_payload(const std::vector<uint8_t>& payload, uint8_t& flags,
                                uint16_t& width, uint16_t& height);

std::vector<uint8_t> make_video_frame_payload(uint32_t seq, uint64_t timestamp_us,
                                              bool is_keyframe, const uint8_t* data,
                                              size_t len);
bool parse_video_frame_payload(const std::vector<uint8_t>& payload, VideoFrameInfo& info);

}  // namespace proto
