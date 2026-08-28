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
#include "tray_icon.hpp"
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

std::string g_config_path;
HANDLE g_reload_event = nullptr;
std::atomic<bool> g_config_dialog_open{false};
std::atomic<int> g_max_encode_width{1920};

// Push quality + resolution settings through the pipeline. The capturer owns
// the encoded size (native desktop capped by max_encode_width), so the
// encoder is always initialised with what the capturer will actually emit.
void configure_pipeline(int fps, int bitrate_kbps, int max_encode_width) {
    if (!g_capturer || !g_encoder) {
        return;
    }
    EncoderConfig ec;
    ec.fps = fps;
    ec.bitrate_kbps = bitrate_kbps;
    ec.max_encode_width = max_encode_width;
    g_capturer->configure(ec);
    g_capturer->encode_size(&ec.width, &ec.height);
    g_encoder->initialize(ec);
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], n);
    return w;
}

gui::ConfigUi make_ui(const config::ClientConfig& c) {
    gui::ConfigUi ui;
    ui.server_ip = c.server_ip;
    ui.server_port = c.server_port;
    ui.secret_key = c.secret_key;
    ui.device_name = c.device_name;
    ui.control_password = c.control_password;
    ui.config_path = g_config_path;
    return ui;
}

// Tray click / second-instance double-click: raise the config dialog of the
// running agent; a save schedules a hot reload in the supervisor loop.
void open_config_dialog() {
    bool expected = false;
    if (!g_config_dialog_open.compare_exchange_strong(expected, true)) {
        return;  // dialog already open
    }
    gui::ConfigUi ui = make_ui(config::ClientConfig::load(g_config_path));
    ui.save_mode = gui::SaveMode::SaveAndApply;
    gui::show_config_gui_async(std::move(ui), [](const gui::ConfigUi& done) {
        g_config_dialog_open.store(false);
        if (done.saved && g_reload_event) {
            SetEvent(g_reload_event);
        }
    });
}

std::string exe_dir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? "." : s.substr(0, pos);
}

// UIPI silently drops SendInput aimed at elevated windows, so an agent that
// starts without admin rights cannot drive Task Manager, UAC prompts or admin
// consoles on the remote desktop.
bool process_is_elevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD len = 0;
        GetTokenInformation(token, TokenElevation, &elevated, sizeof(elevated),
                            &len);
        CloseHandle(token);
    }
    return elevated != FALSE;
}

const bool g_elevated = process_is_elevated();

// A user who right-clicks agent.exe and picks "Run as administrator" would
// otherwise lose the single-instance race against the limited copy, so the
// elevated copy retires same-path instances before claiming the mutex.
bool retire_limited_instances() {
    wchar_t self_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self_path, MAX_PATH)) {
        return false;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool killed = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snap, &entry); ok;
         ok = Process32NextW(snap, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId()) {
            continue;
        }
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                                      PROCESS_TERMINATE,
                                  FALSE, entry.th32ProcessID);
        if (!proc) {
            continue;
        }
        wchar_t other[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, other, &len) &&
            _wcsicmp(other, self_path) == 0 && TerminateProcess(proc, 0)) {
            killed = true;
        }
        CloseHandle(proc);
    }
    CloseHandle(snap);
    return killed;
}

// Starts a second agent with admin rights and steps aside; the child passes
// --takeover so it waits for this process to release the single-instance mutex.
void relaunch_elevated() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        return;
    }
    HINSTANCE result = ShellExecuteW(nullptr, L"runas", path,
                                     L"--background --takeover", nullptr,
                                     SW_HIDE);
    if (reinterpret_cast<INT_PTR>(result) > 32) {
        mlog::info("Elevated agent launched; this limited instance is exiting");
        g_state.running.store(false);
    } else {
        mlog::warn("Elevated restart declined (user cancelled UAC or blocked by policy)");
    }
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
            uint16_t width_cap = 0;
            if (proto::parse_start_stream_payload(payload, fps, bitrate,
                                                  width_cap)) {
                if (width_cap == 0) {
                    // Older control centers omit the cap: keep the configured one.
                    width_cap = static_cast<uint16_t>(g_max_encode_width.load());
                }
                int old_fps = g_state.target_fps.exchange(fps);
                int old_br = g_state.target_bitrate_kbps.exchange(bitrate);
                int old_width = g_max_encode_width.exchange(width_cap);
                // Reconfigure the encoder when quality parameters change so
                // the server's quality preset takes effect immediately.
                if (old_fps != fps || old_br != bitrate || old_width != width_cap) {
                    configure_pipeline(fps, bitrate, width_cap);
                }
            }
            g_state.streaming.store(true);
            mlog::info("Stream started by server (fps=" +
                      std::to_string(g_state.target_fps.load()) + ", bitrate=" +
                      std::to_string(g_state.target_bitrate_kbps.load()) + "kbps, cap=" +
                      std::to_string(g_max_encode_width.load()) + ")");
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
                static uint8_t logged_kinds = 0;
                uint8_t kind_bit = static_cast<uint8_t>(1 << static_cast<int>(ev.kind));
                if (ev.kind != proto::InputKind::MouseMove &&
                    (logged_kinds & kind_bit) == 0) {
                    logged_kinds |= kind_bit;
                    mlog::info("First input event injected (kind=" +
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
    uint8_t flags = g_elevated ? proto::kRegisterFlagElevated : 0;
    auto payload = proto::make_register_payload(
        dev_id, name, static_cast<uint16_t>(width), static_cast<uint16_t>(height),
        flags);
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

    LARGE_INTEGER freq_q;
    QueryPerformanceFrequency(&freq_q);
    const double us_per_tick = 1e6 / static_cast<double>(freq_q.QuadPart);
    LARGE_INTEGER t0{}, t1{};

    uint64_t win_start_ms = GetTickCount64();
    uint64_t cap_us = 0, enc_us = 0, send_us = 0;
    uint64_t captures = 0, encodes = 0, sends = 0, send_fails = 0;

    auto log_window = [&]() {
        uint64_t now = GetTickCount64();
        double secs = (now - win_start_ms) / 1000.0;
        if (secs < 4.5 || captures == 0) {
            if (secs >= 4.5) {  // idle desktop: still report once per window
                mlog::info("Perf 5s: idle (no new desktop frames)");
                win_start_ms = now;
            }
            return;
        }
        uint64_t skips = g_encoder->exchange_skips();
        char line[256];
        snprintf(line, sizeof(line),
                 "Perf 5s: fps=%.1f cap=%.1fms enc=%.1fms send=%.1fms "
                 "enc=%llu skips=%llu netfail=%llu",
                 encodes / secs, cap_us / captures / 1000.0,
                 enc_us / (encodes ? encodes : 1) / 1000.0,
                 send_us / (sends ? sends : 1) / 1000.0,
                 static_cast<unsigned long long>(encodes),
                 static_cast<unsigned long long>(skips),
                 static_cast<unsigned long long>(send_fails));
        mlog::info(line);
        cap_us = enc_us = send_us = 0;
        captures = encodes = sends = send_fails = 0;
        win_start_ms = now;
    };

    while (g_state.running.load()) {
        if (!g_state.streaming.load() || !g_connection->is_connected()) {
            logged_first = false;
            Sleep(100);
            continue;
        }

        int fps = g_state.target_fps.load();
        if (fps <= 0) fps = 30;

        QueryPerformanceCounter(&t0);
        bool captured = g_capturer->capture_frame(frame, 1000 / fps);
        QueryPerformanceCounter(&t1);
        if (captured) {
            cap_us += static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) *
                                            us_per_tick);
            ++captures;
            QueryPerformanceCounter(&t0);
            bool encoded = g_encoder->is_initialized() &&
                           g_encoder->encode_frame(frame);
            QueryPerformanceCounter(&t1);
            enc_us += static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) *
                                            us_per_tick);
            if (encoded && !frame.h264_data.empty()) {
                ++encodes;
                uint32_t seq = g_state.frame_seq.fetch_add(1) + 1;
                auto payload = proto::make_video_frame_payload(
                    seq, frame.timestamp_us, frame.is_keyframe,
                    frame.h264_data.data(), frame.h264_data.size());
                QueryPerformanceCounter(&t0);
                if (g_connection->send(proto::MessageType::VideoFrame, payload)) {
                    ++sends;
                } else {
                    ++send_fails;
                }
                QueryPerformanceCounter(&t1);
                send_us += static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) *
                                                 us_per_tick);
                if (!logged_first) {
                    logged_first = true;
                    mlog::info("Streaming active: first frame " +
                               std::to_string(frame.h264_data.size()) + " bytes, " +
                               std::to_string(frame.width) + "x" +
                               std::to_string(frame.height) + " of desktop " +
                               std::to_string(frame.source_width) + "x" +
                               std::to_string(frame.source_height));
                }
            }
        }
        log_window();
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
    bool takeover = false;
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
    args.takeover = has_flag("--takeover");
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

constexpr wchar_t kAutostartTaskName[] = L"MyRemote Agent";

// Runs a console command to completion; returns its exit code (-1 on failure).
int run_command(const std::wstring& command_line) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_line = command_line;
    if (!CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

}  // namespace

// Logon-time autostart must run elevated: a Run-key agent starts with a
// filtered token and can then never drive elevated windows remotely.
bool set_autostart(bool enable) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring action = enable
                              ? L" /Create /F /SC ONLOGON /RL HIGHEST /TR \"\\\"" +
                                    std::wstring(path) + L"\\\" --background\\\"\""
                              : L" /Delete /F";
    int code = run_command(L"schtasks" + action + L" /TN \"" +
                           kAutostartTaskName + L"\"");

    // Retire the legacy Run key so it cannot start a second, limited agent.
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"MyRemoteAgent");
        RegCloseKey(key);
    }
    return code == 0;
}

namespace {

// A filtered token cannot register a highest-privilege logon task, so hand the
// job to a one-shot elevated sibling when needed.
void request_autostart_install() {
    if (g_elevated) {
        bool ok = set_autostart(true);
        MessageBoxW(nullptr,
                    ok ? L"已安装开机自启：登录时以管理员权限运行。"
                       : L"开机自启安装失败：需要管理员权限。",
                    L"MyRemote",
                    MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        return;
    }
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        return;
    }
    ShellExecuteW(nullptr, L"runas", path, L"--install-autostart", nullptr,
                  SW_HIDE);
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    Args args = parse_command_line();
    if (args.install_autostart || args.uninstall_autostart) {
        bool enable = args.install_autostart;
        bool ok = set_autostart(enable);
        printf("MyRemote autostart %s: %s\n", enable ? "install" : "remove",
               ok ? "ok" : "FAILED");
        if (!ok && enable) {
            printf("Hint: run \"agent.exe --install-autostart\" from an "
                   "elevated prompt so the agent can control elevated windows.\n");
        }
        return ok ? 0 : 1;
    }

    // Physical-pixel metrics everywhere (capture/encoder/screen size).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::string dir = exe_dir();
    g_config_path =
        args.config_path.empty() ? dir + "/config.json" : args.config_path;

    // Exactly one agent process, acquired before any UI is shown. A takeover
    // instance retries because the limited agent it replaces is still exiting;
    // an elevated instance instead retires that limited agent outright.
    bool replaced_instance = false;
    HANDLE instance_mutex = nullptr;
    const int attempts = args.takeover ? 40 : (g_elevated ? 20 : 1);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        instance_mutex = CreateMutexW(nullptr, TRUE,
                                      L"MyRemoteAgent_SingleInstance");
        if (instance_mutex && GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
        if (instance_mutex) {
            CloseHandle(instance_mutex);
            instance_mutex = nullptr;
        }
        if (attempt == 0 && g_elevated && !args.takeover) {
            replaced_instance = retire_limited_instances();
        }
        Sleep(100);
    }
    if (!instance_mutex) {
        if (!args.background) {
            HWND h = nullptr;
            for (int i = 0; i < 20 && !h; ++i) {
                h = FindWindowW(TrayIcon::kWndClass, nullptr);
                if (!h) Sleep(100);
            }
            if (h) {
                PostMessageW(h, TrayIcon::WM_SHOW_CONFIG, 0, 0);
            } else {
                MessageBoxW(nullptr,
                            L"MyRemote Agent 已在运行（见系统托盘图标）。",
                            L"MyRemote", MB_OK | MB_ICONINFORMATION);
            }
        }
        return 0;
    }

    if (args.console) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    mlog::init(dir + "/" + "agent.log");
    mlog::info("MyRemote agent starting");
    if (replaced_instance) {
        mlog::info("Took over from a limited agent instance of the same path");
    }
    mlog::info(g_elevated
                   ? "Elevation: yes (can drive elevated windows)"
                   : "Elevation: no - SendInput into elevated windows (Task "
                     "Manager, UAC, admin consoles) is dropped by UIPI; use the "
                     "tray menu or a scheduled task to run elevated");

    config::ClientConfig cfg = config::ClientConfig::load(g_config_path);
    if (!args.ip_override.empty()) cfg.server_ip = args.ip_override;
    if (args.port_override > 0) cfg.server_port = args.port_override;

    if (args.config_ui) {
        gui::ConfigUi ui = make_ui(cfg);
        ui.save_mode = gui::SaveMode::SaveOnly;
        return gui::run_config_gui(ui) ? 0 : 1;
    }
    if (!args.background) {
        // First double-click on a fresh machine: configure, then run.
        gui::ConfigUi ui = make_ui(cfg);
        ui.save_mode = gui::SaveMode::SaveAndRun;
        if (!gui::run_config_gui(ui)) {
            mlog::info("Configuration cancelled, agent not started");
            return 0;
        }
        cfg = config::ClientConfig::load(g_config_path);
    }

    g_control_password = cfg.control_password;
    std::string dev_id = device::make_device_id();
    mlog::info("Device id: " + dev_id + ", target server: " + cfg.server_ip + ":" +
              std::to_string(cfg.server_port));

    g_reload_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    TrayIcon tray;
    TrayIcon::Actions tray_actions;
    tray_actions.open_config = open_config_dialog;
    tray_actions.quit = [] { g_state.running.store(false); };
    tray_actions.install_autostart = request_autostart_install;
    if (!g_elevated) {
        tray_actions.elevate = relaunch_elevated;
    }
    bool tray_ok = tray.start(std::move(tray_actions));
    if (!tray_ok) {
        mlog::warn("Tray icon unavailable, running headless");
    }
    tray.set_tooltip(to_wide(cfg.device_name.empty()
                                 ? device::default_device_name()
                                 : cfg.device_name) +
                     L" | 连接中");

    g_capturer = std::make_unique<DesktopCapturer>();
    if (!g_capturer->initialize(0)) {
        mlog::error("Desktop capture initialization failed");
        tray.stop();
        return 1;
    }
    mlog::info(std::string("Capture backend: ") +
               (g_capturer->using_bitblt_fallback() ? "BitBlt (DXGI unavailable)"
                                                   : "DXGI Desktop Duplication"));
    g_max_encode_width.store(cfg.max_encode_width);
    g_encoder = std::make_unique<VideoEncoder>();
    configure_pipeline(g_state.target_fps.load(),
                       g_state.target_bitrate_kbps.load(),
                       cfg.max_encode_width);

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

    // Supervisor: connect + register with exponential backoff; the 200ms
    // wake keeps reload latency low even while backing off.
    AutoReconnect reconnect;
    reconnect.set_callback([&]() {
        return establish_session(cfg, dev_id);
    });

    int backoff_ms = 1000;
    DWORD next_try_tick = 0;
    std::wstring last_tip;
    while (g_state.running.load()) {
        if (WaitForSingleObject(g_reload_event, 200) == WAIT_OBJECT_0) {
            mlog::info("Config saved while running; reloading");
            g_connection->disconnect();
            g_state.registered.store(false);
            g_state.streaming.store(false);
            cfg = config::ClientConfig::load(g_config_path);
            if (!args.ip_override.empty()) cfg.server_ip = args.ip_override;
            if (args.port_override > 0) cfg.server_port = args.port_override;
            g_control_password = cfg.control_password;
            g_connection->set_encryption_key(crypto::derive_key(cfg.secret_key));
            g_max_encode_width.store(cfg.max_encode_width);
            configure_pipeline(g_state.target_fps.load(),
                               g_state.target_bitrate_kbps.load(),
                               cfg.max_encode_width);
            backoff_ms = 1000;
            next_try_tick = 0;
        }

        if (!g_connection->is_connected() || !g_state.registered.load()) {
            if (static_cast<int32_t>(GetTickCount() - next_try_tick) >= 0) {
                g_state.streaming.store(false);
                g_state.registered.store(false);
                if (reconnect.try_once()) {
                    backoff_ms = 1000;
                } else {
                    next_try_tick = GetTickCount() + backoff_ms;
                    backoff_ms = std::min(backoff_ms * 2, 30000);
                }
            }
        }

        std::wstring tip = to_wide(cfg.device_name.empty()
                                       ? device::default_device_name()
                                       : cfg.device_name) +
                           L" | " + to_wide(cfg.server_ip + ":" +
                                            std::to_string(cfg.server_port)) +
                           (g_state.registered.load() ? L" | 已注册"
                                                      : L" | 连接/重试中") +
                           (g_elevated ? L"" : L" | 受限");
        if (tip != last_tip) {
            tray.set_tooltip(tip);
            last_tip = tip;
        }
    }

    g_state.running.store(false);
    if (stream_thread.joinable()) stream_thread.join();
    g_heartbeat.reset();
    g_connection.reset();
    tray.stop();
    mlog::info("MyRemote agent stopped");
    return 0;
}
