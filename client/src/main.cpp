// MyRemote Agent (被控端)
// Runs silently in the background; actively connects out to the server.
// The client never listens or accepts connections (one-way network rule).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "auto_reconnect.hpp"
#include "config.hpp"
#include "connection.hpp"
#include "desktop_capture.hpp"
#include "device_id.hpp"
#include "heartbeat.hpp"
#include "input_simulator.hpp"
#include "log.hpp"
#include "messages.hpp"
#include "video_encoder.hpp"

namespace {

struct AgentState {
    std::atomic<bool> running{true};
    std::atomic<bool> registered{false};
    std::atomic<bool> streaming{false};
    std::atomic<int> register_result{-1};  // -1 pending, 0 ok, 1 rejected, 2 full
    std::atomic<int> target_fps{30};
    std::atomic<int> target_bitrate_kbps{2048};
    std::atomic<uint32_t> frame_seq{0};
};

AgentState g_state;
std::unique_ptr<Connection> g_connection;
std::unique_ptr<DesktopCapturer> g_capturer;
std::unique_ptr<VideoEncoder> g_encoder;
std::unique_ptr<HeartbeatKeeper> g_heartbeat;
InputSimulator g_input;

std::string exe_dir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? "." : s.substr(0, pos);
}

void on_message(proto::MessageType type, std::vector<uint8_t> payload) {
    switch (type) {
        case proto::MessageType::RegisterAck: {
            proto::RegisterStatus status = proto::RegisterStatus::Rejected;
            if (proto::parse_register_ack_payload(payload, status)) {
                g_state.register_result.store(static_cast<int>(status));
            } else {
                g_state.register_result.store(1);
            }
            break;
        }
        case proto::MessageType::StartStream: {
            uint8_t fps = 30;
            uint16_t bitrate = 2048;
            if (proto::parse_start_stream_payload(payload, fps, bitrate)) {
                g_state.target_fps.store(fps);
                g_state.target_bitrate_kbps.store(bitrate);
            }
            g_state.streaming.store(true);
            mlog::info("Stream started by server (fps=" +
                      std::to_string(g_state.target_fps.load()) + ")");
            break;
        }
        case proto::MessageType::StopStream:
            g_state.streaming.store(false);
            mlog::info("Stream stopped by server");
            break;
        case proto::MessageType::RequestKeyframe:
            g_encoder->force_keyframe();
            break;
        case proto::MessageType::InputEvent:
            // TODO(M5): parse and inject mouse/keyboard events
            mlog::info("Input event received (" + std::to_string(payload.size()) + " bytes)");
            break;
        case proto::MessageType::AuthChallenge:
            // TODO(M7): secondary password response
            mlog::warn("Auth challenge received (not yet supported)");
            break;
        default:
            mlog::warn("Unknown message type: " +
                      std::to_string(static_cast<int>(type)));
            break;
    }
}

bool send_register(const config::ClientConfig& cfg, const std::string& dev_id) {
    std::string name = cfg.device_name.empty() ? device::default_device_name()
                                               : cfg.device_name;
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    auto payload = proto::make_register_payload(
        dev_id, name, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
    return g_connection->send(proto::MessageType::Register, payload);
}

// Connect, register and wait for the server ack. Returns true when accepted.
bool establish_session(const config::ClientConfig& cfg, const std::string& dev_id) {
    if (!g_connection->connect(cfg.server_ip, cfg.server_port)) {
        return false;
    }

    g_state.register_result.store(-1);
    if (!send_register(cfg, dev_id)) {
        g_connection->disconnect();
        return false;
    }

    // Wait up to 5s for RegisterAck.
    for (int i = 0; i < 50; ++i) {
        if (!g_connection->is_connected()) {
            return false;
        }
        int result = g_state.register_result.load();
        if (result >= 0) {
            if (result == static_cast<int>(proto::RegisterStatus::Ok)) {
                g_state.registered.store(true);
                mlog::info("Registered with server (device_id=" + dev_id + ")");
                return true;
            }
            mlog::error("Server rejected registration (status=" +
                       std::to_string(result) + ")");
            g_connection->disconnect();
            return false;
        }
        Sleep(100);
    }

    mlog::error("Registration ack timeout");
    g_connection->disconnect();
    return false;
}

void stream_loop() {
    CapturedFrame frame;
    while (g_state.running.load()) {
        if (!g_state.streaming.load() || !g_connection->is_connected()) {
            Sleep(100);
            continue;
        }

        int fps = g_state.target_fps.load();
        if (fps <= 0) fps = 30;

        if (g_capturer->capture_frame(frame, 1000 / fps)) {
            bool encoded = g_encoder->is_initialized() &&
                           g_encoder->encode_frame(frame.raw_bgra.data(), frame.width,
                                                   frame.height, &frame);
            if (encoded && !frame.h264_data.empty()) {
                uint32_t seq = g_state.frame_seq.fetch_add(1) + 1;
                auto payload = proto::make_video_frame_payload(
                    seq, frame.timestamp_us, frame.is_keyframe,
                    frame.h264_data.data(), frame.h264_data.size());
                g_connection->send(proto::MessageType::VideoFrame, payload);
            }
        }
    }
}

}  // namespace

namespace {

struct Args {
    bool console = false;
    std::string ip_override;
    int port_override = 0;
    std::string config_path;
};

Args parse_command_line() {
    Args args;
    std::string cmd = GetCommandLineA();
    auto has_flag = [&cmd](const std::string& flag) {
        return cmd.find(flag) != std::string::npos;
    };
    auto get_value = [&cmd](const std::string& flag) -> std::string {
        size_t pos = cmd.find(flag);
        if (pos == std::string::npos) return {};
        pos = cmd.find_first_not_of(" \t", pos + flag.size());
        if (pos == std::string::npos) return {};
        size_t end = cmd.find_first_of(" \t", pos);
        return cmd.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    };

    args.console = has_flag("--console");
    args.ip_override = get_value("--ip");
    std::string port = get_value("--port");
    if (!port.empty()) {
        args.port_override = std::atoi(port.c_str());
    }
    args.config_path = get_value("--config");
    return args;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    Args args = parse_command_line();

    if (args.console) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    std::string dir = exe_dir();
    mlog::init(dir + "/" + "agent.log");
    mlog::info("MyRemote agent starting");

    std::string config_path = args.config_path.empty() ? dir + "\config.json"
                                                       : args.config_path;
    config::ClientConfig cfg = config::ClientConfig::load(config_path);
    if (!args.ip_override.empty()) cfg.server_ip = args.ip_override;
    if (args.port_override > 0) cfg.server_port = args.port_override;

    std::string dev_id = device::make_device_id();
    mlog::info("Device id: " + dev_id + ", target server: " + cfg.server_ip + ":" +
              std::to_string(cfg.server_port));

    g_capturer = std::make_unique<DesktopCapturer>();
    if (!g_capturer->initialize(0)) {
        mlog::error("Desktop capture initialization failed");
        return 1;
    }
    EncoderConfig enc_cfg;
    enc_cfg.fps = 30;
    enc_cfg.width = GetSystemMetrics(SM_CXSCREEN);
    enc_cfg.height = GetSystemMetrics(SM_CYSCREEN);
    g_capturer->configure(enc_cfg);

    g_encoder = std::make_unique<VideoEncoder>();
    g_encoder->initialize(enc_cfg);

    g_connection = std::make_unique<Connection>();
    g_connection->set_message_callback(on_message);

    g_heartbeat = std::make_unique<HeartbeatKeeper>();
    g_heartbeat->start(1000, []() {
        if (!g_state.registered.load() || !g_connection->is_connected()) {
            return true;  // nothing to do yet
        }
        return g_connection->send(proto::MessageType::Heartbeat,
                                  proto::make_heartbeat_payload());
    });

    std::thread stream_thread(stream_loop);

    // Supervisor: connect + register with exponential backoff.
    AutoReconnect reconnect;
    reconnect.set_callback([&]() {
        return establish_session(cfg, dev_id);
    });

    int backoff_ms = 1000;
    while (g_state.running.load()) {
        if (!g_connection->is_connected() || !g_state.registered.load()) {
            g_state.streaming.store(false);
            g_state.registered.store(false);
            if (reconnect.try_once()) {
                backoff_ms = 1000;
            } else {
                Sleep(backoff_ms);
                backoff_ms = std::min(backoff_ms * 2, 30000);
            }
        } else {
            Sleep(500);
        }
    }

    g_state.running.store(false);
    if (stream_thread.joinable()) stream_thread.join();
    g_heartbeat.reset();
    g_connection.reset();
    mlog::info("MyRemote agent stopped");
    return 0;
}
