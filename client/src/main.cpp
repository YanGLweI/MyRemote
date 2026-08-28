// MyRemote Agent (被控端)
// Runs silently in the background; actively connects out to the server.
// The client never listens or accepts connections (one-way network rule).

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "auto_reconnect.hpp"
#include "config.hpp"
#include "config_gui.hpp"
#include "connection.hpp"
#include "crypto.hpp"
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
    std::mutex cfg_mutex;
    std::atomic<uint32_t> frame_seq{0};
};

AgentState g_state;
std::string g_control_password;
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
                int old_fps = g_state.target_fps.exchange(fps);
                int old_br = g_state.target_bitrate_kbps.exchange(bitrate);
                // Reconfigure the encoder when quality parameters change so
                // the server's quality preset takes effect immediately.
                if (g_encoder && (old_fps != fps || old_br != bitrate)) {
                    EncoderConfig ec;
                    ec.fps = fps;
                    ec.bitrate_kbps = bitrate;
                    ec.width = GetSystemMetrics(SM_CXSCREEN);
                    ec.height = GetSystemMetrics(SM_CYSCREEN);
                    g_encoder->shutdown();
                    g_encoder->initialize(ec);
                    g_encoder->force_keyframe();
                }
            }
            g_state.streaming.store(true);
            mlog::info("Stream started by server (fps=" +
                      std::to_string(g_state.target_fps.load()) + ", bitrate=" +
                      std::to_string(g_state.target_bitrate_kbps.load()) + "kbps)");
            break;
        }
        case proto::MessageType::StopStream:
            g_state.streaming.store(false);
            mlog::info("Stream stopped by server");
            break;
        case proto::MessageType::RequestKeyframe:
            g_encoder->force_keyframe();
            break;
        case proto::MessageType::InputEvent: {
            proto::InputEvent ev;
            if (proto::parse_input_event(payload, ev)) {
                static bool logged_input = false;
                if (!logged_input) {
                    logged_input = true;
                    mlog::info("Input event received and injected (kind=" +
                               std::to_string(static_cast<int>(ev.kind)) + ")");
                }
                if (ev.kind == proto::InputKind::Key && ev.pressed) {
                    char hex[8];
                    sprintf(hex, "%02X", ev.vk);
                    mlog::info(std::string("Key injected: vk=0x") + hex +
                               (ev.extended ? " extended" : ""));
                }
                g_input.handle(ev);
            }
            break;
        }
        case proto::MessageType::AuthChallenge: {
            auto hmac = crypto::hmac_sha256(g_control_password, payload);
            g_connection->send(proto::MessageType::AuthResponse, hmac);
            mlog::info("Auth challenge answered (control password check)");
            break;
        }
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
    bool logged_first = false;
    while (g_state.running.load()) {
        if (!g_state.streaming.load() || !g_connection->is_connected()) {
            logged_first = false;
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
                if (!logged_first) {
                    logged_first = true;
                    mlog::info("Streaming active: first frame " +
                               std::to_string(frame.h264_data.size()) + " bytes, " +
                               std::to_string(frame.width) + "x" +
                               std::to_string(frame.height));
                }
            }
        }
    }
}

}  // namespace

namespace {

struct Args {
    bool console = false;
    bool config_ui = false;
    bool background = false;
    bool install_autostart = false;
    bool uninstall_autostart = false;
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
        size_t pos = 0;
        while (true) {
            pos = cmd.find(flag, pos);
            if (pos == std::string::npos) return {};
            size_t after = pos + flag.size();
            if (after >= cmd.size() || cmd[after] == ' ' || cmd[after] == '\t' ||
                cmd[after] == '=') {
                break;
            }
            pos = after;  // e.g. "--config-ui" must not match "--config"
        }
        size_t vpos = pos + flag.size();
        vpos = cmd.find_first_not_of(" \t=", vpos);
        if (vpos == std::string::npos) return {};
        size_t end = cmd.find_first_of(" \t", vpos);
        return cmd.substr(vpos, end == std::string::npos ? std::string::npos
                                                          : end - vpos);
    };

    args.console = has_flag("--console");
    args.config_ui = has_flag("--config-ui");
    args.background = has_flag("--background");
    args.install_autostart = has_flag("--install-autostart");
    args.uninstall_autostart = has_flag("--uninstall-autostart");
    args.ip_override = get_value("--ip");
    std::string port = get_value("--port");
    if (!port.empty()) {
        args.port_override = std::atoi(port.c_str());
    }
    args.config_path = get_value("--config");
    return args;
}

}  // namespace

namespace {
// Terminates other running copies of this exact executable so a freshly
// configured instance can take over (config changes need a reconnect).
void kill_other_agent_instances() {
    wchar_t my_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, my_path, MAX_PATH);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"agent.exe") != 0 ||
                pe.th32ProcessID == GetCurrentProcessId()) {
                continue;
            }
            HANDLE proc = OpenProcess(
                PROCESS_TERMINATE | SYNCHRONIZE |
                    PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE, pe.th32ProcessID);
            if (!proc) {
                continue;
            }
            wchar_t path[MAX_PATH] = {};
            DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, path, &len) &&
                _wcsicmp(path, my_path) == 0) {
                TerminateProcess(proc, 0);
                WaitForSingleObject(proc, 2000);
            }
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}
}  // namespace

namespace {
void set_autostart(bool enable) {
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\Microsoft\Windows\CurrentVersion\Run", 0,
                      KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string cmd = std::string("\"") + path + "\" --background";
        RegSetValueExA(key, "MyRemoteAgent", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>(cmd.size() + 1));
    } else {
        RegDeleteValueA(key, "MyRemoteAgent");
    }
    RegCloseKey(key);
}
}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    Args args = parse_command_line();
    if (args.install_autostart) {
        set_autostart(true);
        return 0;
    }
    if (args.uninstall_autostart) {
        set_autostart(false);
        return 0;
    }

    // Double-click (no --background) opens the configuration GUI; after
    // "save and run" this same process continues as the background agent.
    if (args.config_ui || !args.background) {
        std::string dir = exe_dir();
        std::string config_path = args.config_path.empty()
                                      ? dir + "/config.json"
                                      : args.config_path;
        config::ClientConfig current = config::ClientConfig::load(config_path);
        gui::ConfigUi ui;
        ui.server_ip = current.server_ip;
        ui.server_port = current.server_port;
        ui.secret_key = current.secret_key;
        ui.device_name = current.device_name;
        ui.control_password = current.control_password;
        ui.config_path = config_path;
        ui.run_after_save = !args.config_ui;
        bool saved = gui::run_config_gui(ui);
        if (args.config_ui) {
            return saved ? 0 : 1;
        }
        if (!saved) {
            return 0;
        }
        kill_other_agent_instances();
    }

    if (args.console) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    std::string dir = exe_dir();
    mlog::init(dir + "/" + "agent.log");
    mlog::info("MyRemote agent starting");

    std::string config_path = args.config_path.empty() ? dir + "/config.json"
                                                       : args.config_path;
    config::ClientConfig cfg = config::ClientConfig::load(config_path);
    if (!args.ip_override.empty()) cfg.server_ip = args.ip_override;
    if (args.port_override > 0) cfg.server_port = args.port_override;

    g_control_password = cfg.control_password;
    std::string dev_id = device::make_device_id();
    mlog::info("Device id: " + dev_id + ", target server: " + cfg.server_ip + ":" +
              std::to_string(cfg.server_port));

    HANDLE single_instance =
        CreateMutexW(nullptr, TRUE, L"MyRemoteAgent_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        mlog::error("Another agent instance is already running, exiting");
        return 1;
    }
    (void)single_instance;

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
    g_connection->set_encryption_key(crypto::derive_key(cfg.secret_key));

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
