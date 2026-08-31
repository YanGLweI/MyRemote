// MyRemote Agent (被控端)
// Runs silently in the background; actively connects out to the server.
// The client never listens or accepts connections (one-way network rule).

#include <windows.h>

#include <shellapi.h>
#include <tlhelp32.h>
#include <wtsapi32.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

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
#include "tray_proxy.hpp"
#include "tray_proxies.hpp"
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
// "本机不想被远控"，由托盘翻转。它刻意**不落盘**：重启之后这台机器应该重新可控，
// 而开机自启是另一码事——这里停的只是这条隧道，进程和服务都还活着，所以图标还在，
// 同一个菜单就是回来的路。
std::atomic<bool> g_paused{false};
// 托盘线程只能提出请求；写配置文件的是主循环，免得两个线程抢同一个文件。
std::atomic<bool> g_hide_tray{false};
// 菜单跑在托盘自己那个消息线程上，而 TrayIcon::stop() 要 join 它——自杀式死锁。
// 所以"退出"也只是个请求：由主循环先删图标、再停服务。
std::atomic<bool> g_quit_requested{false};
// 这台机器在 SCM 眼里是什么形状。由监管循环喂，托盘菜单只读它：查 SCM 这件事
// 一旦放上消息线程，M20 那种"菜单再也不出来"就回来了。
enum ServiceShape { NoService = 0, ServiceStopped = 1, ServiceRunning = 2 };
std::atomic<int> g_service_shape{NoService};

int read_service_shape() {
    if (!svc::is_installed()) {
        return NoService;
    }
    return svc::is_running() ? ServiceRunning : ServiceStopped;
}
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
    ui.tray_icon = c.tray_icon;
    ui.max_encode_width = c.max_encode_width;
    ui.config_path = g_config_path;
    return ui;
}

// A config.json that exists but yields not one known setting is not
// "unconfigured" - it is a file somebody loses the moment a dialog pre-filled
// with defaults gets saved. Those dialogs say so and stay shut. A missing file
// is the other thing: that is what a machine nobody has set up looks like, and
// it stays editable.
bool config_is_editable(config::LoadStatus status) {
    if (status != config::LoadStatus::Unreadable) {
        return true;
    }
    mlog::warn("Config exists but holds no setting we know; not editing it: " +
               g_config_path);
    const std::wstring text =
        L"读不到配置文件里的任何设置：\n" + to_wide(g_config_path) +
        L"\n\n在这个窗口里保存会把默认值覆盖上去，所以它没有打开。"
        L"请先修正这个文件，或者删掉它再重新配置。";
    MessageBoxW(nullptr, text.c_str(), L"MyRemote 配置", MB_OK | MB_ICONERROR);
    return false;
}

// Tray click / second-instance double-click: raise the config dialog of the
// running agent; a save schedules a hot reload in the supervisor loop.
void open_config_dialog() {
    bool expected = false;
    if (!g_config_dialog_open.compare_exchange_strong(expected, true)) {
        return;  // dialog already open
    }
    config::LoadStatus status = config::LoadStatus::Missing;
    config::ClientConfig read =
        config::ClientConfig::load(g_config_path, &status);
    if (!config_is_editable(status)) {
        g_config_dialog_open.store(false);
        return;
    }
    gui::ConfigUi ui = make_ui(read);
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

// The tray proxy (M19) shares this image path but must never be a takeover
// target: the sweep below would otherwise kill the operator's tray on every
// elevated double-click. Identify it by its command line, and treat "could not
// read it" as untouchable - the cost of sparing a rival agent is one retry,
// the cost of killing a proxy is a painted icon that never answers again.
#ifndef ProcessCommandLineInformation
#define ProcessCommandLineInformation static_cast<PROCESSINFOCLASS>(60)
#endif
enum class ProxyProbe { NotProxy, IsProxy, Unknown };
ProxyProbe probe_tray_proxy(DWORD pid) {
    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS,
                                                         PVOID, ULONG, PULONG);
    static auto* nt_query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!nt_query) {
        return ProxyProbe::Unknown;
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return ProxyProbe::Unknown;
    }
    std::wstring cmd;
    ULONG need = 0;
    std::vector<unsigned char> buffer(512);
    NTSTATUS st = nt_query(proc, ProcessCommandLineInformation, buffer.data(),
                           static_cast<ULONG>(buffer.size()), &need);
    if (st == static_cast<NTSTATUS>(0xC0000004L) && need > buffer.size()) {
        buffer.resize(need);
        st = nt_query(proc, ProcessCommandLineInformation, buffer.data(), need,
                      &need);
    }
    CloseHandle(proc);
    if (st < 0) {
        return ProxyProbe::Unknown;
    }
    const auto* us = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
    if (!us->Buffer || !us->Length) {
        return ProxyProbe::Unknown;
    }
    cmd.assign(us->Buffer, us->Length / sizeof(wchar_t));
    return cmd.find(L"--tray-proxy") != std::wstring::npos ? ProxyProbe::IsProxy
                                                           : ProxyProbe::NotProxy;
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
        // A tray proxy is a child of the host, so parentage alone settles it
        // without reading anything out of the other process.
        if (entry.th32ParentProcessID == GetCurrentProcessId() ||
            (skip_pid && entry.th32ParentProcessID == skip_pid)) {
            continue;
        }
        if (probe_tray_proxy(entry.th32ProcessID) != ProxyProbe::NotProxy) {
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

// The host runs as SYSTEM inside someone else's session, so it also leaves a
// one-line status file behind: that is what `agent.exe --service-state` prints,
// and it is the only record of which desktop the host thinks it is on.
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
    const std::string proxies = trayproxies::proxy_sessions();
    char line[256];
    snprintf(line, sizeof(line),
             "pid=%lu session=%lu desktop=%s capture=%s size=%dx%d registered=%d paused=%d proxies=%s\n",
             static_cast<unsigned long>(GetCurrentProcessId()),
             static_cast<unsigned long>(session_id),
             desktop.empty() ? "unknown" : desktop.c_str(),
             g_capturer && g_capturer->using_bitblt_fallback() ? "bitblt" : "dxgi",
             width, height, g_state.registered.load() ? 1 : 0,
             g_paused.load() ? 1 : 0,
             proxies.empty() ? "none" : proxies.c_str());
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
    bool tray_proxy = false;
    bool background = false;
    bool version = false;
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
    // The shell's own parser, not a scan over GetCommandLineA(). The session
    // host hands every tray proxy a quoted --config path (tray_proxies.cpp),
    // and this scanner kept the quote marks as part of the value and cut the
    // value at its first space - so on an installed machine the proxy tried to
    // open a path that cannot exist, and the config fell back to defaults in
    // silence. Wide tokens also drop the ACP round trip that mangled a
    // non-ASCII path, which is decoded from UTF-8 downstream.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return args;
    }
    std::set<std::string> flags;
    std::map<std::string, std::string> values;
    for (int i = 1; i < argc; ++i) {
        const std::string tok = win32util::wide_to_utf8(argv[i]);
        if (tok.rfind("--", 0) != 0) {
            continue;  // somebody else's value, or something we never ask about
        }
        const size_t eq = tok.find('=');
        if (eq != std::string::npos) {
            values[tok.substr(0, eq)] = tok.substr(eq + 1);
            continue;
        }
        flags.insert(tok);
        if ((tok == "--ip" || tok == "--port" || tok == "--config") &&
            i + 1 < argc) {
            values[tok] = win32util::wide_to_utf8(argv[++i]);
        }
    }
    LocalFree(argv);

    // Exact token: --service can no longer be matched by --service-state, and
    // --config can no longer be matched by --config-ui, with no boundary scan.
    auto has_flag = [&flags](const std::string& flag) {
        return flags.count(flag) != 0;
    };
    auto get_value = [&values](const std::string& flag) -> std::string {
        const auto it = values.find(flag);
        return it == values.end() ? std::string() : it->second;
    };

    args.console = has_flag("--console");
    args.config_ui = has_flag("--config-ui");
    args.tray_proxy = has_flag("--tray-proxy");
    args.background = has_flag("--background");
    args.version = has_flag("--version");
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
        // Spawned by the service. It must not elevate, must not block on a
        // dialog, and must follow whichever desktop owns the keyboard.
        // The tray is deliberately *not* switched off here: the host is created
        // on Winsta0\Default (service.cpp), desktop affinity is per-thread, and
        // the follower only moves the capture thread - so its icon is naturally
        // invisible while the machine is sitting at the logon screen, which is
        // the exposure this line used to guard against by having no icon at all.
        args.background = true;
        args.no_elevate = true;
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
    // Answering "which build is this" must not start anything: no elevation, no
    // mutex, no config, and above all no log file - a version query that leaves
    // agent.log behind would make a machine look alive when nothing runs.
    if (args.version) {
        win32util::attach_parent_console();
        printf("MyRemote agent %s\n", MYREMOTE_VERSION);
        return 0;
    }
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

    // M19 per-session tray process. It is not an agent instance: no elevation
    // prompt, no single-instance mutex, no yielding to the service - it only
    // draws the icon for its session and forwards menu actions to the host.
    if (args.tray_proxy) {
        return trayproxy::run(args.config_path);
    }

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
        // 锁在别的 agent 手里。如果拿着它的是服务宿主（SYSTEM），"把它叫醒"这条路
        // 走不通：UIPI 会把 Medium→System 的 WM_APP 悄悄丢掉，而 PostMessage 照样
        // 返回 TRUE——点了没反应，比报错更难解释。所以交给下面让位那一支，让点击的
        // 人以自己的身份打开配置界面。
        const bool host_owns_machine =
            !g_session_host && !args.force && !args.background &&
            svc::is_installed() && svc::is_running();
        if (!host_owns_machine) {
            if (!args.background) {
                HWND h = nullptr;
                for (int i = 0; i < 20 && !h; ++i) {
                    // The tray window is a hidden top-level tool window, so it
                    // is found by class + title from another process. This lands
                    // on the session's own tray proxy, which opens the dialog as
                    // that user - the one thing a SYSTEM host could not do.
                    h = FindWindowW(TrayIcon::kWndClass, TrayIcon::kWndTitle);
                    if (!h) Sleep(100);
                }
                if (h) {
                    PostMessageW(h, TrayIcon::WM_SHOW_CONFIG, 0, 0);
                } else {
                    MessageBoxW(nullptr,
                                L"MyRemote Agent 已在运行，但当前看不到它的托盘图标"
                                L"（可能被隐藏了）。请从开始菜单打开"
                                L"“配置界面”，勾回“显示托盘图标”。",
                                L"MyRemote", MB_OK | MB_ICONINFORMATION);
                }
            }
            return 0;
        }
    }

    if (args.console) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    mlog::init(g_log_dir + "\\" + "agent.log");
    SetUnhandledExceptionFilter(crash_filter);
    mlog::info("MyRemote agent starting");
    mlog::info("Config file: " + g_config_path +
               (paths.config_present ? " (found)" : " (missing)"));
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

    config::LoadStatus cfg_status = config::LoadStatus::Missing;
    config::ClientConfig cfg = config::ClientConfig::load(g_config_path, &cfg_status);
    if (cfg_status == config::LoadStatus::Unreadable && (g_session_host || args.background)) {
        // No dialog to open for a headless process, but "I am running on the
        // defaults" is not something a log file should keep to itself.
        mlog::warn("The config file holds no setting we know; running on defaults: " +
                   g_config_path);
    }
    if (!args.ip_override.empty()) cfg.server_ip = args.ip_override;
    if (args.port_override > 0) cfg.server_port = args.port_override;

    if (args.config_ui) {
        if (!config_is_editable(cfg_status)) {
            return 1;
        }
        gui::ConfigUi ui = make_ui(cfg);
        ui.save_mode = gui::SaveMode::SaveOnly;
        return gui::run_config_gui(ui) ? 0 : 1;
    }
    // 只在服务**真的在跑**时让位。托盘"退出"之后服务是停的，这时双击必须把客户端
    // 重新跑起来，而不是弹一个只读的编辑器然后退出。
    if (!g_session_host && !args.force && svc::is_installed() && svc::is_running()) {
        mlog::info("The MyRemoteAgent service owns this machine; this instance "
                   "exits so the tunnel stays with the hosted process. Use "
                   "--force to override.");
        if (!args.background) {
            if (!config_is_editable(cfg_status)) {
                return 1;
            }
            gui::ConfigUi ui = make_ui(cfg);
            ui.save_mode = gui::SaveMode::SaveOnly;
            gui::run_config_gui(ui);
        }
        return 0;
    }
    if (!instance_mutex) {
        // 上面那两支之间宿主可能刚好退了。再抢一次锁并**持有**它；还抢不到就到此
        // 为止——两个 agent 抢同一个设备号，比"这次没起来"难查得多。
        instance_mutex = CreateMutexW(nullptr, TRUE, L"MyRemoteAgent_SingleInstance");
        if (!instance_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (instance_mutex) {
                CloseHandle(instance_mutex);
            }
            mlog::info("Another agent took the instance lock first; this instance exits");
            return 0;
        }
    }
    if (!args.background) {
        // First double-click on a fresh machine: configure, then run.
        if (!config_is_editable(cfg_status)) {
            return 1;
        }
        gui::ConfigUi ui = make_ui(cfg);
        ui.save_mode = gui::SaveMode::SaveAndRun;
        if (!gui::run_config_gui(ui)) {
            mlog::info("Configuration cancelled, agent not started");
            return 0;
        }
        cfg = config::ClientConfig::load(g_config_path, &cfg_status);
        if (cfg_status != config::LoadStatus::Read) {
            mlog::warn("Saved, but the agent cannot read that file back: " +
                       g_config_path);
        }
    }

    g_control_password = cfg.control_password;
    std::string dev_id = device::make_device_id();
    mlog::info("Device id: " + dev_id + ", target server: " + cfg.server_ip + ":" +
              std::to_string(cfg.server_port));

    g_reload_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_input_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 便携形态的托盘一直在进程里；服务态从 M19 起不再由宿主自己画——图标改由
    // 每个登录会话里一个用户态代理进程来画（RDP 与控制台都看得见），命令经命名
    // 管道回到这里。所以进程内托盘只对便携形态生效。
    TrayIcon tray;
    TrayIcon::Actions tray_actions;
    bool tray_visible = false;
    std::wstring last_tip;
    const bool tray_possible =
        !args.no_tray && !args.background && !g_session_host;
    auto show_tray = [&]() {
        tray_visible = tray.start(tray_actions);
        if (!tray_visible) {
            mlog::warn("Tray icon unavailable, running headless");
            return;
        }
        tray.set_tooltip(to_wide(cfg.device_name.empty()
                                     ? device::default_device_name()
                                     : cfg.device_name) +
                         L" | 连接中");
        // 新建的图标只有上面这一句，循环里的读数必须重新推一遍。
        last_tip.clear();
    };
    if (tray_possible) {
        tray_actions.open_config = open_config_dialog;
        tray_actions.toggle_pause = [] {
            const bool now = !g_paused.load();
            g_paused.store(now);
            mlog::info(now
                           ? "Remote control paused by the local user; dropping the tunnel"
                           : "Remote control resumed by the local user");
        };
        tray_actions.paused = [] { return g_paused.load(); };
        tray_actions.config_pointless_now = [] { return g_secure_desktop.load(); };
        tray_actions.hide_icon = [] { g_hide_tray.store(true); };
        tray_actions.quit = [] { g_state.running.store(false); };
        // 装过服务之后，前两条要么没用（宿主本来就是 SYSTEM），要么有害（计划任务
        // 和服务抢同一个设备号）；服务装了但停着——正是托盘点了「退出」之后——才给
        // 最后一条。回调一律挂着，给不给由下面这两个谓词在弹菜单那一刻决定：形状会
        // 在进程活着的时候变（点了启用、别人用 services.msc 停了、覆盖安装），启动时
        // 算一次会让菜单说完谎就一直说到进程结束。
        tray_actions.install_autostart = request_autostart_install;
        if (!g_elevated) {
            tray_actions.elevate = relaunch_elevated;
        }
        tray_actions.start_service = [] {
            std::wstring why;
            if (svc::start(&why)) {
                mlog::info("Service re-enabled from the tray");
                return;
            }
            const std::wstring text =
                L"无法启动 MyRemoteAgent 服务：" + why +
                L"\n\n启动服务需要管理员权限。";
            mlog::warn("The tray could not start the service: " +
                       win32util::wide_to_utf8(why.c_str()));
            MessageBoxW(nullptr, text.c_str(), L"MyRemote",
                        MB_OK | MB_ICONWARNING);
        };
        tray_actions.show_autostart_group = [] {
            return g_service_shape.load() == NoService;
        };
        tray_actions.show_start_service = [] {
            return g_service_shape.load() == ServiceStopped;
        };
        g_service_shape.store(read_service_shape());
        if (cfg.tray_icon) {
            show_tray();
        }
    }
    if (g_session_host && !args.no_tray) {
        trayproxies::CommandSink sink;
        sink.pause = [] {
            g_paused.store(true);
            mlog::info("Remote control paused from a session tray; dropping the tunnel");
        };
        sink.resume = [] {
            g_paused.store(false);
            mlog::info("Remote control resumed from a session tray");
        };
        sink.hide = [] { g_hide_tray.store(true); };
        sink.quit = [] { g_quit_requested.store(true); };
        sink.save_config = [](const std::string& json) {
            // The proxy runs user-IL and may not be able to write the agent
            // directory; the host writes on its behalf and the mtime poll
            // below picks the change up as a normal hot reload.
            config::LoadStatus status = config::LoadStatus::Unreadable;
            const config::ClientConfig incoming =
                config::ClientConfig::from_json(json, &status);
            if (status != config::LoadStatus::Read) {
                // save() rewrites the whole file, so a payload that carries no
                // settings would write the defaults over somebody's real ones.
                mlog::warn("Tray proxy sent a config we could not read; nothing written");
                return false;
            }
            return config::ClientConfig::save(incoming, g_config_path);
        };
        trayproxies::start(std::move(sink), g_config_path);
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
    DWORD next_shape_tick = 0;
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
            config::LoadStatus restatus = config::LoadStatus::Missing;
            config::ClientConfig fresh =
                config::ClientConfig::load(g_config_path, &restatus);
            if (restatus != config::LoadStatus::Read) {
                // Swapping in the code defaults here would move a live tunnel to
                // 127.0.0.1 because a file went missing; keeping the settings the
                // agent is actually running with is the steadier and louder call.
                mlog::warn("The config file is gone or holds no setting we know; "
                           "keeping the settings this agent runs with");
            } else {
                cfg = fresh;
            }
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
            // "显示托盘图标"这个复选框是藏掉图标后唯一的回头路，所以它必须
            // 在运行中的进程上也立刻生效，而不是等下一次开机。
            if (tray_possible && cfg.tray_icon != tray_visible) {
                if (cfg.tray_icon) {
                    show_tray();
                } else {
                    tray.stop();
                    tray_visible = false;
                }
            }
        }

        if (tray_possible &&
            static_cast<int32_t>(GetTickCount() - next_shape_tick) >= 0) {
            // Here rather than in ShowMenu: this thread may afford a slow call,
            // the tray's message thread may not - that is precisely how M20
            // started. One sample per second is soon enough for a menu the human
            // has to right-click to open, and it makes an item that was just
            // acted on disappear by the next popup.
            next_shape_tick = GetTickCount() + 1000;
            g_service_shape.store(read_service_shape());
        }

        if (g_quit_requested.exchange(false)) {
            // 先删图标再停服务：停服务会让监管把我们收掉，图标要是还挂着，
            // 机器上就留下一个点不动的幽灵。服务态的图标在各会话代理里。
            tray.stop();
            trayproxies::stop();
            std::wstring why;
            if (svc::stop(&why)) {
                mlog::info("Service stopped from the tray; it starts again on next boot");
            } else {
                mlog::warn("The tray could not stop the service: " +
                           win32util::wide_to_utf8(why.c_str()));
            }
            break;
        }
        if (g_hide_tray.exchange(false)) {
            tray.stop();
            tray_visible = false;
            config::ClientConfig hidden = cfg;
            hidden.tray_icon = false;
            if (!config::ClientConfig::save(hidden, g_config_path)) {
                // 图标这次是藏了，但下次开机它还会回来——这句话要说出口，
                // 不能让用户以为"隐藏"是永久的。
                mlog::warn("Tray icon hidden for now, but the setting did not persist: " +
                           g_config_path);
            }
            // 内存里这一份必须跟着变：广播给各会话代理的 tray_icon 读的就是它，
            // 只改副本等于永远不说"这枚图标不该存在"，图标既不自己退、监管还会
            // 继续补生——这正是"点了隐藏却没藏掉"的全部经过。
            cfg = hidden;
            // 自己刚写的这一份不是"操作者又改了设置"，不该触发一次重载。
            config_written = config_stamp();
        }
        // 广播在重连**之前**：try_once() 会占满 10s 连接超时，放在它后面等于"控制端
        // 联系不上的机器，图标要等一轮超时才肯消失"。registered 因此晚一轮（200ms）。
        trayproxies::broadcast_state(
            g_paused.load(), g_state.registered.load(), cfg.tray_icon,
            cfg.server_ip + ":" + std::to_string(cfg.server_port),
            cfg.device_name.empty() ? device::default_device_name()
                                    : cfg.device_name);

        if (g_paused.load()) {
            // 暂停只关隧道：进程与服务都活着，图标也还在，同一个菜单就是回来的路。
            if (g_connection->is_connected()) {
                g_connection->disconnect();
                g_state.registered.store(false);
                g_state.streaming.store(false);
                backoff_ms = 1000;
            }
            // 把"下次尝试"始终留在近处，恢复时不必等一轮退避。
            next_try_tick = GetTickCount() + 1000;
        } else if (!g_connection->is_connected() || !g_state.registered.load()) {
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
                           (g_paused.load()
                                ? L" | 已停止，本机不被远控"
                                : (g_state.registered.load() ? L" | 已注册"
                                                             : L" | 连接/重试中")) +
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
    trayproxies::stop();
    mlog::info("MyRemote agent stopped");
    return 0;
}
