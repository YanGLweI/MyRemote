// MyRemote Agent (被控端)
// Runs silently in the background; actively connects out to the server.
// The client never listens or accepts connections (one-way network rule).

#include <windows.h>

#include <tlhelp32.h>
#include <wtsapi32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "auto_reconnect.hpp"
#include "config.hpp"
#include "config_gui.hpp"
#include "connection.hpp"
#include "crypto.hpp"
#include "desktop.hpp"
#include "desktop_capture.hpp"
#include "device_id.hpp"
#include "heartbeat.hpp"
#include "input_simulator.hpp"
#include "log.hpp"
#include "messages.hpp"
#include "service.hpp"
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
std::string g_log_dir;
HANDLE g_reload_event = nullptr;
std::atomic<bool> g_config_dialog_open{false};
std::atomic<int> g_max_encode_width{1920};

// Started by the service: no UI of any kind, and the desktop it has to work on
// is whichever one currently owns the keyboard.
bool g_session_host = false;
bool g_follow_desktop = false;
std::atomic<bool> g_secure_desktop{false};
// Whether this process may follow the logon screen at all, as opposed to
// whether it is on one right now.
bool g_can_use_secure_desktop = false;
std::atomic<uint8_t> g_reported_flags{0};

// SendInput is delivered to whatever desktop the *calling* thread is attached
// to, and only one thread may ever hold that affinity, so the network thread
// hands events over instead of injecting them itself.
std::mutex g_inputs_mutex;
std::deque<proto::InputEvent> g_pending_inputs;
HANDLE g_input_wake = nullptr;

void queue_input(const proto::InputEvent& ev) {
    {
        std::lock_guard<std::mutex> lock(g_inputs_mutex);
        // Only the newest pointer position matters; presses and releases do not.
        if (ev.kind == proto::InputKind::MouseMove && !g_pending_inputs.empty() &&
            g_pending_inputs.back().kind == proto::InputKind::MouseMove) {
            g_pending_inputs.back() = ev;
        } else {
            g_pending_inputs.push_back(ev);
        }
    }
    if (g_input_wake) {
        SetEvent(g_input_wake);
    }
}

// Injects what the capture thread's desktop can actually receive.
int drain_inputs() {
    int injected = 0;
    while (injected < 64) {
        proto::InputEvent ev;
        {
            std::lock_guard<std::mutex> lock(g_inputs_mutex);
            if (g_pending_inputs.empty()) {
                break;
            }
            ev = g_pending_inputs.front();
            g_pending_inputs.pop_front();
        }
        g_input.handle(ev);
        if (ev.kind == proto::InputKind::Key && ev.pressed) {
            char hex[8];
            sprintf(hex, "%02X", ev.vk);
            mlog::info(std::string("Key injected: vk=0x") + hex +
                       (ev.extended ? " extended" : ""));
        }
        ++injected;
    }
    return injected;
}

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
    if (ec.width <= 0 || ec.height <= 0) {
        // No desktop yet (logon screen / detached session); the pipeline is
        // reconfigured as soon as frames become available.
        mlog::info("No desktop to encode yet, pipeline not configured");
        return;
    }
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
    ui.max_encode_width = c.max_encode_width;
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

// Capture and injected input only reach the session this process runs in. When
// an RDP client is closed the session is left detached with no desktop on the
// console, which looks like a hung agent from the control side.
void log_session_state() {
    DWORD session_id = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
    DWORD console_id = WTSGetActiveConsoleSessionId();
    std::string msg = "Session " + std::to_string(session_id);
    if (console_id == 0xFFFFFFFF) {
        msg += "; no session attached to the physical console";
    } else if (console_id != session_id) {
        msg += "; console runs session " + std::to_string(console_id) +
               " - this agent is not in it";
    } else {
        msg += " = console session";
    }
    std::string desk = win32util::input_desktop_name();
    msg += "; input desktop \"" + (desk.empty() ? std::string("unknown") : desk) +
           "\"";
    msg += win32util::process_is_system()
               ? "; running as SYSTEM"
               : (g_elevated ? "; running elevated" : "; running limited");
    mlog::info(msg);
}

// The session host publishes its own pid next to agent.log. Only trust it while
// the service is actually installed, or a stale file would spare a real
// duplicate.
DWORD hosted_agent_pid() {
    if (!svc::is_installed() || g_log_dir.empty()) {
        return 0;
    }
    std::wstring path = win32util::utf8_to_wide(g_log_dir + "\\host.status");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    char text[256] = {};
    DWORD read = 0;
    ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
    CloseHandle(file);
    unsigned long pid = 0;
    return sscanf(text, "pid=%lu", &pid) == 1 ? static_cast<DWORD>(pid) : 0;
}

// Any second copy of this exact image in this session would fight for the
// tunnel and the single-instance mutex. The scan is deliberately limited to
// the caller's own session: the service and its host share one image path, and
// a host that killed its own supervisor would be restarted by the SCM forever.
bool retire_same_path_instances(DWORD skip_pid = 0) {
    wchar_t self_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self_path, MAX_PATH)) {
        return false;
    }
    DWORD own_session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &own_session);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool killed = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snap, &entry); ok;
         ok = Process32NextW(snap, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId() ||
            entry.th32ProcessID == skip_pid) {
            continue;
        }
        DWORD other_session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &other_session) ||
            other_session != own_session) {
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

// Our command line without the leading executable path.
std::wstring command_line_params() {
    std::wstring cmd = GetCommandLineW();
    size_t i = 0;
    if (i < cmd.size() && cmd[i] == L'"') {
        i = cmd.find(L'"', 1);
        if (i == std::wstring::npos) return {};
        ++i;
    } else {
        i = cmd.find(L' ');
        if (i == std::wstring::npos) return {};
    }
    while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t')) ++i;
    return cmd.substr(i);
}

// Starts a second agent with admin rights. The tray action passes --takeover so
// the child waits for this process to release the single-instance mutex.
bool start_elevated_agent(const std::wstring& params, int show_cmd) {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        return false;
    }
    HINSTANCE result = ShellExecuteW(nullptr, L"runas", path, params.c_str(),
                                     nullptr, show_cmd);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

// Tray entry: hand over to an elevated copy and step aside.
void relaunch_elevated() {
    if (start_elevated_agent(L"--background --takeover --no-elevate", SW_HIDE)) {
        mlog::info("Elevated agent launched; this limited instance is exiting");
        g_state.running.store(false);
    } else {
        mlog::warn("Elevated restart declined (user cancelled UAC or blocked by policy)");
    }
}

// The mode list answers a query, and is re-sent after every set attempt so the
// control centre reads the outcome from the current mode it carries.
void send_display_modes() {
    std::vector<std::pair<uint16_t, uint16_t>> wire;
    for (const auto& m : win32util::list_display_modes()) {
        wire.emplace_back(static_cast<uint16_t>(m.first),
                          static_cast<uint16_t>(m.second));
    }
    g_connection->send(
        proto::MessageType::DisplayModes,
        proto::make_display_modes_payload(
            static_cast<uint16_t>(GetSystemMetrics(SM_CXSCREEN)),
            static_cast<uint16_t>(GetSystemMetrics(SM_CYSCREEN)), wire));
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
            std::optional<uint16_t> wire_cap;
            if (proto::parse_start_stream_payload(payload, fps, bitrate,
                                                  wire_cap)) {
                // Only an absent field (old control center) falls back to the
                // configured cap; an explicit 0 means "native desktop size".
                uint16_t width_cap = wire_cap.value_or(
                    static_cast<uint16_t>(g_max_encode_width.load()));
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
                    mlog::info("First input event received (kind=" +
                               std::to_string(static_cast<int>(ev.kind)) + ")");
                }
                queue_input(ev);
            }
            break;
        }
        case proto::MessageType::AuthChallenge: {
            auto hmac = crypto::hmac_sha256(g_control_password, payload);
            g_connection->send(proto::MessageType::AuthResponse, hmac);
            mlog::info("Auth challenge answered (control password check)");
            break;
        }
        case proto::MessageType::LockWorkstation: {
            // Ctrl+Alt+Del cannot be injected, so this is the honest way back to
            // a credential prompt.
            const BOOL ok = LockWorkStation();
            mlog::info(ok ? "Workstation locked; logon screen is up"
                          : "LockWorkStation refused by this session");
            break;
        }
        case proto::MessageType::Ping: {
            // Pure echo: the stamp is the control centre's own clock reading, and
            // nothing on this side is measured, compared, or acted upon. Carrying
            // no agent-side sample is what lets the control centre time a round
            // trip without needing our two clocks to agree about anything.
            uint64_t t0_us = 0;
            if (!proto::parse_ping_payload(payload, t0_us)) {
                break;
            }
            g_connection->send(proto::MessageType::Pong,
                               proto::make_pong_payload(t0_us));
            break;
        }
        case proto::MessageType::QueryDisplayModes: {
            send_display_modes();
            break;
        }
        case proto::MessageType::SetDisplayMode: {
            uint16_t w = 0, h = 0;
            if (!proto::parse_set_display_mode_payload(payload, w, h)) {
                break;
            }
            const int old_w = GetSystemMetrics(SM_CXSCREEN);
            const int old_h = GetSystemMetrics(SM_CYSCREEN);
            const LONG result = win32util::change_display_mode(w, h);
            if (result == DISP_CHANGE_SUCCESSFUL) {
                mlog::info("Display mode set to " + std::to_string(w) + "x" +
                           std::to_string(h) + " (was " + std::to_string(old_w) +
                           "x" + std::to_string(old_h) + ")");
            } else {
                mlog::warn("Display mode " + std::to_string(w) + "x" +
                           std::to_string(h) + " refused, code " +
                           std::to_string(result));
            }
            send_display_modes();
            break;
        }
        default:
            mlog::warn("Unknown message type: " +
                      std::to_string(static_cast<int>(type)));
            break;
    }
}

uint8_t capability_flags() {
    uint8_t f = 0;
    if (g_elevated) {
        f |= proto::kRegisterFlagElevated;
    }
    if (win32util::process_is_system()) {
        f |= proto::kFlagIsSystem | proto::kRegisterFlagElevated;
    }
    if (g_session_host) {
        f |= proto::kFlagServiceHost;
    }
    if (g_can_use_secure_desktop) {
        f |= proto::kFlagSecureDesktop;
    }
    if (g_secure_desktop.load()) {
        f |= proto::kFlagLogonScreen;
    }
    DWORD session_id = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
    if (session_id == WTSGetActiveConsoleSessionId()) {
        f |= proto::kFlagConsoleOwner;
    }
    return f;
}

// Silent unless something actually changed, so it is cheap to call per beat.
// A second Register would not do: the server retires the previous session for
// the same device id, which is this one.
void send_state_report() {
    if (!g_connection || !g_state.registered.load() ||
        !g_connection->is_connected()) {
        return;
    }
    const uint8_t f = capability_flags();
    if (g_reported_flags.exchange(f) == f) {
        return;
    }
    char hex[8];
    snprintf(hex, sizeof(hex), "%02X", f);
    mlog::info(std::string("Capability change reported (flags=0x") + hex + ")");
    g_connection->send(
        proto::MessageType::StateReport,
        proto::make_state_report_payload(
            f, static_cast<uint16_t>(GetSystemMetrics(SM_CXSCREEN)),
            static_cast<uint16_t>(GetSystemMetrics(SM_CYSCREEN))));
}

bool send_register(const config::ClientConfig& cfg, const std::string& dev_id) {
    std::string name = cfg.device_name.empty() ? device::default_device_name()
                                               : cfg.device_name;
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    auto payload = proto::make_register_payload(
        dev_id, name, static_cast<uint16_t>(width), static_cast<uint16_t>(height),
        capability_flags());
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

// The host has no tray and must never show a window, so it leaves a one-line
// status file behind instead: that is what `agent.exe --service-state` prints.
void write_host_status(const std::string& desktop) {
    if (!g_session_host || g_log_dir.empty()) {
        return;
    }
    int width = 0;
    int height = 0;
    if (g_capturer) {
        g_capturer->encode_size(&width, &height);
    }
    DWORD session_id = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
    char line[256];
    snprintf(line, sizeof(line),
             "pid=%lu session=%lu desktop=%s capture=%s size=%dx%d registered=%d\n",
             static_cast<unsigned long>(GetCurrentProcessId()),
             static_cast<unsigned long>(session_id),
             desktop.empty() ? "unknown" : desktop.c_str(),
             g_capturer && g_capturer->using_bitblt_fallback() ? "bitblt" : "dxgi",
             width, height, g_state.registered.load() ? 1 : 0);
    std::wstring path = win32util::utf8_to_wide(g_log_dir + "\\host.status");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    CloseHandle(file);
}

void stream_loop() {
    CapturedFrame frame;
    bool logged_first = false;
    int reported_w = 0;
    int reported_h = 0;

    LARGE_INTEGER freq_q;
    QueryPerformanceFrequency(&freq_q);
    const double us_per_tick = 1e6 / static_cast<double>(freq_q.QuadPart);
    LARGE_INTEGER t0{}, t1{}, loop_start{};

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

    // This thread is the only one allowed to hold desktop affinity, so it is
    // also the only one allowed to inject input.
    win32util::DesktopFollower follower;
    ULONGLONG last_status_ms = 0;
    auto refresh_status = [&]() {
        ULONGLONG now = GetTickCount64();
        if (!g_session_host || now - last_status_ms < 1000) {
            return;
        }
        last_status_ms = now;
        write_host_status(g_follow_desktop ? follower.name()
                                           : win32util::input_desktop_name());
    };

    while (g_state.running.load()) {
        bool desktop_changed = false;
        if (g_follow_desktop) {
            follower.update(nullptr, &desktop_changed);
            if (desktop_changed) {
                const std::string desktop = follower.name();
                mlog::info("Now attached to desktop \"" + desktop + "\" (" +
                           (follower.on_secure_desktop() ? "secure" : "user") + ")");
                g_secure_desktop.store(follower.on_secure_desktop());
                if (g_capturer) {
                    g_capturer->on_desktop_switched();
                }
                reported_w = 0;
                reported_h = 0;
                if (g_encoder) {
                    g_encoder->force_keyframe();
                }
                configure_pipeline(g_state.target_fps.load(),
                                   g_state.target_bitrate_kbps.load(),
                                   g_max_encode_width.load());
            }
        }

        if (!g_state.streaming.load() || !g_connection->is_connected()) {
            logged_first = false;
            reported_w = 0;
            reported_h = 0;
            WaitForSingleObject(g_input_wake, 100);
            drain_inputs();
            refresh_status();
            continue;
        }

        int fps = g_state.target_fps.load();
        if (fps <= 0) fps = 30;
        QueryPerformanceCounter(&loop_start);

        QueryPerformanceCounter(&t0);
        bool captured = g_capturer->capture_frame(frame, 1000 / fps);
        QueryPerformanceCounter(&t1);
        if (captured) {
            cap_us += static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) *
                                            us_per_tick);
            ++captures;
            if (frame.source_width != reported_w || frame.source_height != reported_h) {
                reported_w = frame.source_width;
                reported_h = frame.source_height;
                g_connection->send(
                    proto::MessageType::DisplayChanged,
                    proto::make_display_changed_payload(
                        static_cast<uint16_t>(frame.source_width),
                        static_cast<uint16_t>(frame.source_height)));
                mlog::info("Reported desktop resize to server (" +
                           std::to_string(frame.source_width) + "x" +
                           std::to_string(frame.source_height) + ")");
            }
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
        // Inject after the capture, so the pointer and keystrokes land on the
        // same desktop the frame that is going out was taken from.
        drain_inputs();
        refresh_status();
        // Neither the BitBlt fallback nor the "no desktop yet" early-out ever
        // blocks, so the frame budget has to be honoured here; AcquireNextFrame
        // already consumed it on the duplication path.
        QueryPerformanceCounter(&t1);
        const int budget_ms = 1000 / fps;
        double spent_ms =
            (t1.QuadPart - loop_start.QuadPart) * us_per_tick / 1000.0;
        if (budget_ms > 0 && spent_ms < budget_ms) {
            Sleep(static_cast<DWORD>(budget_ms - spent_ms));
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
    bool no_elevate = false;
    bool service = false;
    bool session_host = false;
    bool no_tray = false;
    bool follow_desktop = false;
    bool force = false;
    bool install_service = false;
    bool uninstall_service = false;
    bool start_service = false;
    bool stop_service = false;
    bool service_state = false;
    std::string ip_override;
    int port_override = 0;
    std::string config_path;
};

Args parse_command_line() {
    Args args;
    std::string cmd = GetCommandLineA();
    // Boundary-aware: "--service" must not match "--service-state".
    auto has_flag = [&cmd](const std::string& flag) {
        size_t pos = 0;
        while (true) {
            pos = cmd.find(flag, pos);
            if (pos == std::string::npos) {
                return false;
            }
            size_t after = pos + flag.size();
            if (after >= cmd.size() || cmd[after] == ' ' || cmd[after] == '\t' ||
                cmd[after] == '=') {
                return true;
            }
            pos = after;
        }
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
    args.no_elevate = has_flag("--no-elevate");
    args.service = has_flag("--service");
    args.session_host = has_flag("--session-host");
    args.no_tray = has_flag("--no-tray");
    args.follow_desktop = has_flag("--follow-desktop");
    args.force = has_flag("--force");
    args.install_service = has_flag("--install-service");
    args.uninstall_service = has_flag("--uninstall-service");
    args.start_service = has_flag("--start-service");
    args.stop_service = has_flag("--stop-service");
    args.service_state = has_flag("--service-state");
    if (args.session_host) {
        // Spawned by the service: nobody is watching this machine, and a
        // window on the secure desktop is visible to whoever walks past it.
        args.background = true;
        args.no_elevate = true;
        args.no_tray = true;
        args.follow_desktop = true;
    }
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

// A filtered token cannot register a highest-privilege logon task, so hand the
// job to a one-shot elevated sibling when needed.
void request_autostart_install() {
    if (g_elevated) {
        bool ok = win32util::set_autostart(true);
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

// Without this an access violation is indistinguishable from a machine that
// was never reachable: the process just stops existing.
LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    char line[160];
    snprintf(line, sizeof(line), "UNHANDLED EXCEPTION code=0x%08lX at %p",
             static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
             info->ExceptionRecord->ExceptionAddress);
    mlog::error(line);
    return EXCEPTION_EXECUTE_HANDLER;
}

// One-shot command-line operations: nothing here starts the agent itself.
int run_service_cli(const Args& args) {
    if (args.install_autostart || args.uninstall_autostart) {
        bool enable = args.install_autostart;
        bool ok = win32util::set_autostart(enable);
        printf("MyRemote autostart %s: %s\n", enable ? "install" : "remove",
               ok ? "ok" : "FAILED");
        if (!ok && enable) {
            printf("Hint: the logon task needs highest privileges; run this from "
                   "an elevated prompt.\n");
        }
        return ok ? 0 : 1;
    }
    if (args.service_state) {
        printf("%s", svc::query().c_str());
        return 0;
    }

    const bool install = args.install_service;
    const bool remove = args.uninstall_service;
    const bool stop = args.stop_service;
    const char* verb = install ? "install" : remove ? "uninstall"
                       : stop   ? "stop"
                                : "start";
    const wchar_t* flag = install ? L"--install-service"
                        : remove  ? L"--uninstall-service"
                        : stop    ? L"--stop-service"
                                  : L"--start-service";
    if (!g_elevated) {
        // Every one of these is refused to a filtered token.
        std::wstring params = flag;
        params += L" --no-elevate";
        if (start_elevated_agent(params, SW_HIDE)) {
            printf("MyRemoteAgent %s: administrator approval requested.\n", verb);
            printf("Run \"agent.exe --service-state\" afterwards to check.\n");
            return 0;
        }
        printf("MyRemoteAgent %s: FAILED - administrator rights are required.\n",
               verb);
        return 1;
    }

    std::wstring why;
    bool ok = install ? svc::install_or_update(&why)
              : remove ? svc::uninstall(&why)
              : stop   ? svc::stop(&why)
                       : svc::start(&why);
    printf("MyRemoteAgent %s: %s%s%s\n", verb, ok ? "ok" : "FAILED",
           ok ? "" : " - ", ok ? "" : win32util::wide_to_utf8(why.c_str()).c_str());
    if (remove && ok) {
        printf("The configuration in %%ProgramData%%\\MyRemote is kept.\n"
               "Fallback without a service: agent.exe --install-autostart\n");
    }
    return ok ? 0 : 1;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    Args args = parse_command_line();
    // The service dispatcher and the install CLI must be reached before the
    // single-instance mutex and the auto-elevation block: the mutex is
    // session-local, and elevation is meaningless under the SCM.
    if (args.service) {
        return svc::run_as_service();
    }
    if (args.install_service || args.uninstall_service || args.start_service ||
        args.stop_service || args.service_state || args.install_autostart ||
        args.uninstall_autostart) {
        win32util::attach_parent_console();
        return run_service_cli(args);
    }

    // Physical-pixel metrics everywhere (capture/encoder/screen size).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Interactive launch: request admin up front, because a limited agent
    // cannot drive elevated windows remotely. --background must not prompt, or
    // unattended logon startup would hang on the consent dialog.
    bool elevation_declined = false;
    if (!g_elevated && !args.no_elevate && !args.background && !args.config_ui) {
        std::wstring params = command_line_params();
        if (!params.empty()) {
            params += L' ';
        }
        params += L"--no-elevate";
        if (start_elevated_agent(params, SW_SHOWNORMAL)) {
            return 0;  // the elevated copy takes over from here
        }
        elevation_declined = true;
    }

    win32util::AgentPaths paths = win32util::resolve_paths(args.config_path);
    g_config_path = paths.config;
    g_log_dir = paths.log_dir;

    g_session_host = args.session_host;
    g_follow_desktop = args.follow_desktop;
    bool tcb_privilege = false;
    bool debug_privilege = false;
    if (g_session_host) {
        // The service just made this process the owner of the tunnel, so any
        // other copy of this image is now a bug, not a fallback.
        retire_same_path_instances();
        // Following the logon screen is impossible without SeTcbPrivilege:
        // OpenInputDesktop on "Winlogon" is refused to everyone else, which is
        // precisely why the host has to run as SYSTEM.
        tcb_privilege = win32util::enable_privilege(L"SeTcbPrivilege");
        debug_privilege = win32util::enable_privilege(L"SeDebugPrivilege");
        g_can_use_secure_desktop = tcb_privilege;
    }

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
            // Never sweep the service host: the supervisor would relaunch it
            // within two seconds and the two would retire each other forever.
            replaced_instance = retire_same_path_instances(hosted_agent_pid());
            if (!replaced_instance && hosted_agent_pid()) {
                mlog::info("The service session host keeps running; use "
                           "--stop-service first to take its place");
            }
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

    mlog::init(g_log_dir + "\\" + "agent.log");
    SetUnhandledExceptionFilter(crash_filter);
    mlog::info("MyRemote agent starting");
    mlog::info("Config file: " + g_config_path);
    if (g_session_host) {
        mlog::info("Session host started by the MyRemoteAgent service "
                   "(no tray, no dialogs, follows the input desktop)");
        mlog::info(std::string("SeTcbPrivilege: ") +
                   (tcb_privilege ? "yes" : "NO - the logon screen stays dark"));
        mlog::info(std::string("SeDebugPrivilege: ") +
                   (debug_privilege ? "yes" : "no"));
    }
    if (elevation_declined) {
        mlog::warn("UAC prompt dismissed; continuing without admin rights");
    }
    if (replaced_instance) {
        mlog::info("Took over from a limited agent instance of the same path");
    }
    mlog::info(g_elevated
                   ? "Elevation: yes (can drive elevated windows)"
                   : "Elevation: no - SendInput into elevated windows (Task "
                     "Manager, UAC, admin consoles) is dropped by UIPI; use the "
                     "tray menu or a scheduled task to run elevated");
    log_session_state();

    config::ClientConfig cfg = config::ClientConfig::load(g_config_path);
    if (!args.ip_override.empty()) cfg.server_ip = args.ip_override;
    if (args.port_override > 0) cfg.server_port = args.port_override;

    if (args.config_ui) {
        gui::ConfigUi ui = make_ui(cfg);
        ui.save_mode = gui::SaveMode::SaveOnly;
        return gui::run_config_gui(ui) ? 0 : 1;
    }
    if (!g_session_host && !args.force && svc::is_installed()) {
        mlog::info("The MyRemoteAgent service owns this machine; this instance "
                   "exits so the tunnel stays with the hosted process. Use "
                   "--force to override.");
        if (!args.background) {
            gui::ConfigUi ui = make_ui(cfg);
            ui.save_mode = gui::SaveMode::SaveOnly;
            gui::run_config_gui(ui);
        }
        return 0;
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
    g_input_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // A host has no tray and no dialogs: on the secure desktop every window it
    // creates is visible to whoever is standing at the machine.
    TrayIcon tray;
    if (!args.no_tray) {
        TrayIcon::Actions tray_actions;
        tray_actions.open_config = open_config_dialog;
        tray_actions.quit = [] { g_state.running.store(false); };
        tray_actions.install_autostart = request_autostart_install;
        if (!g_elevated) {
            tray_actions.elevate = relaunch_elevated;
        }
        if (!tray.start(std::move(tray_actions))) {
            mlog::warn("Tray icon unavailable, running headless");
        }
        tray.set_tooltip(to_wide(cfg.device_name.empty()
                                     ? device::default_device_name()
                                     : cfg.device_name) +
                         L" | 连接中");
    }

    g_capturer = std::make_unique<DesktopCapturer>();
    if (!g_capturer->initialize(0)) {
        mlog::error("Desktop capture initialization failed");
        if (!g_session_host) {
            tray.stop();
            return 1;
        }
        // A host that quits here takes the machine offline until the next
        // session change; it is cheaper to keep polling for a desktop.
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
        send_state_report();
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
    auto config_stamp = [&]() {
        FILETIME stamp{};
        HANDLE f = CreateFileW(win32util::utf8_to_wide(g_config_path).c_str(),
                               GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            GetFileTime(f, nullptr, nullptr, &stamp);
            CloseHandle(f);
        }
        return stamp;
    };
    FILETIME config_written = config_stamp();
    while (g_state.running.load()) {
        // The tray dialog can only signal its own session; the config file is
        // what a user editing settings has in common with a hosted agent.
        FILETIME stamp = config_stamp();
        if (CompareFileTime(&stamp, &config_written) != 0) {
            config_written = stamp;
            SetEvent(g_reload_event);
        }
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
