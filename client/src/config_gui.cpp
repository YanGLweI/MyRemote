// Native Win32 configuration dialog for the headless agent.
// Opened on double-click (first run), via --config-ui, or from the tray of a
// running agent (show_config_gui_async). Writes config.json; the running
// agent hot-reloads it on SaveAndApply.

#include "config_gui.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "config.hpp"
#include "crypto.hpp"
#include "device_id.hpp"
#include "frame_codec.hpp"
#include "messages.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace gui {
namespace {

enum : int {
    IDC_ED_IP = 2001,
    IDC_ED_PORT,
    IDC_ED_KEY,
    IDC_ED_NAME,
    IDC_ED_PWD,
    IDC_BTN_TEST,
    IDC_BTN_SAVE,
    IDC_BTN_CANCEL,
    IDC_ST_STATUS,
    IDC_ST_FILE,
};

constexpr int kFieldW = 300;
constexpr int kFieldH = 24;
constexpr int kLabelH = 18;
constexpr int kGap = 8;
constexpr int kMargin = 14;
constexpr int kClientW = kMargin * 2 + kFieldW;
constexpr int kClientH = 422;
constexpr wchar_t kWndClass[] = L"MyRemoteConfigWnd";

// Per-dialog state; lives as GWLP_USERDATA of the dialog window.
struct DlgData {
    ConfigUi* cfg = nullptr;
    HFONT font = nullptr;
    float scale = 1.0f;

    int px(int v) const { return static_cast<int>(v * scale + 0.5f); }
};

DlgData* data_of(HWND hwnd) {
    return reinterpret_cast<DlgData*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

std::wstring wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        &w[0], n);
    return w;
}

std::string utf8(HWND hwnd) {
    int n = GetWindowTextLengthW(hwnd);
    if (n <= 0) return {};
    std::wstring w(n + 1, L'\0');
    GetWindowTextW(hwnd, &w[0], n + 1);
    w.resize(n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], m, nullptr, nullptr);
    return s;
}

void set_status(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(hwnd, IDC_ST_STATUS), text.c_str());
}

HWND make_label(DlgData* d, HWND parent, const wchar_t* text, int x, int y,
                int w) {
    HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                             x, y, w, d->px(kLabelH), parent, nullptr, nullptr,
                             nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(d->font), TRUE);
    return h;
}

HWND make_edit(DlgData* d, HWND parent, int id, const std::wstring& text, int x,
               int y, bool password) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    if (password) {
        style |= ES_PASSWORD;
    }
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(), style,
                             x, y, d->px(kFieldW), d->px(kFieldH), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(d->font), TRUE);
    SendMessageW(h, EM_SETLIMITTEXT, 255, 0);
    if (password) {
        SendMessageW(h, EM_SETPASSWORDCHAR, L'*', 0);
    }
    return h;
}

HWND make_button(DlgData* d, HWND parent, int id, const wchar_t* text, int x,
                 int y, int w, int height = 30) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             x, y, d->px(w), d->px(height), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(d->font), TRUE);
    return h;
}

bool valid_port(const std::string& s) {
    if (s.empty() || s.size() > 5) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    long v = std::stol(s);
    return v >= 1 && v <= 65535;
}

// Performs the same connect + encrypted Register handshake the agent uses,
// so the result matches what the real agent would experience. Winsock is
// initialized ref-counted by the OS and never cleaned up here: the agent
// process also uses sockets, and WSACleanup would race with it.
std::wstring test_register(const std::string& host, int port,
                           const std::string& psk, const std::string& dev_name) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return L"Winsock 初始化失败";
    }

    std::string port_str = std::to_string(port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        return L"无法解析服务器地址，请检查 IP 是否填写正确";
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return L"创建套接字失败";
    }

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return L"连接失败：错误码 " +
                   wide(std::to_string(WSAGetLastError())) +
                   L"。请确认服务端已启动，且服务器防火墙放行了该端口";
        }
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(s, &wset);
        timeval tv{4, 0};
        int sel = select(0, nullptr, &wset, nullptr, &tv);
        if (sel <= 0) {
            closesocket(s);
            return L"连接超时：无法到达服务器。请检查 IP/端口，"
                   L"以及服务器防火墙是否放行 TCP " +
                   wide(std::to_string(port)) + L" 入站";
        }
        int err = 0;
        int len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0) {
            closesocket(s);
            return L"连接被拒绝（错误码 " + wide(std::to_string(err)) +
                   L"）。请确认服务端程序正在运行";
        }
    }

    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    int tmo = 4000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo),
               sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo),
               sizeof(tmo));

    // Build the encrypted Register frame exactly like the agent does.
    std::string name = dev_name.empty() ? device::default_device_name() : dev_name;
    auto reg_payload = proto::make_register_payload(
        device::make_device_id(), name,
        static_cast<uint16_t>(GetSystemMetrics(SM_CXSCREEN)),
        static_cast<uint16_t>(GetSystemMetrics(SM_CYSCREEN)));
    std::vector<uint8_t> inner;
    inner.push_back(static_cast<uint8_t>(proto::MessageType::Register));
    inner.insert(inner.end(), reg_payload.begin(), reg_payload.end());
    crypto::AesGcm aes(crypto::derive_key(psk));
    auto frame = proto::encode_frame(proto::MessageType::Encrypted,
                                     aes.encrypt(inner));
    if (::send(s, reinterpret_cast<const char*>(frame.data()),
               static_cast<int>(frame.size()), 0) !=
        static_cast<int>(frame.size())) {
        closesocket(s);
        return L"发送注册请求失败";
    }

    proto::FrameDecoder decoder;
    char buf[4096];
    DWORD deadline = GetTickCount() + 5000;
    std::wstring result;
    while (result.empty() && GetTickCount() < deadline) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) {
            result = L"服务器断开了连接且未回应注册。通常是连接密钥"
                     L"与服务端不一致，请核对双方的 secret_key";
            break;
        }
        if (!decoder.feed(reinterpret_cast<const uint8_t*>(buf),
                          static_cast<size_t>(n))) {
            result = L"服务器返回的数据不符合协议，请确认服务端版本一致";
            break;
        }
        while (decoder.has_frame()) {
            auto fr = decoder.pop_frame();
            std::vector<uint8_t> inner_msg;
            if (fr.type == proto::MessageType::Encrypted) {
                try {
                    inner_msg = aes.decrypt(fr.payload);
                } catch (...) {
                    result = L"无法解密服务器响应：双方连接密钥不一致";
                    break;
                }
            } else {
                inner_msg = std::move(fr.payload);
                inner_msg.insert(inner_msg.begin(),
                                 static_cast<uint8_t>(fr.type));
            }
            if (inner_msg.empty()) continue;
            auto type = static_cast<proto::MessageType>(inner_msg[0]);
            if (type != proto::MessageType::RegisterAck) continue;
            std::vector<uint8_t> ack_payload(inner_msg.begin() + 1,
                                             inner_msg.end());
            proto::RegisterStatus status = proto::RegisterStatus::Rejected;
            if (!proto::parse_register_ack_payload(ack_payload, status)) {
                result = L"注册响应格式错误";
                break;
            }
            switch (status) {
                case proto::RegisterStatus::Ok:
                    result = L"注册成功！服务器已接受本机，配置有效";
                    break;
                case proto::RegisterStatus::ServerFull:
                    result = L"连接成功，但服务器连接数已满，请稍后重试";
                    break;
                default:
                    result = L"连接成功，但服务器拒绝了注册。"
                             L"请检查连接密钥是否与服务端一致";
                    break;
            }
        }
    }
    if (result.empty()) {
        result = L"已建立 TCP 连接，但等待注册响应超时。"
                 L"请确认服务端版本一致且密钥正确";
    }
    closesocket(s);
    return result;
}

LRESULT CALLBACK pwd_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        SendMessageW(GetParent(hwnd), WM_COMMAND, IDC_BTN_SAVE, 0);
        return 0;
    }
    WNDPROC old = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return CallWindowProcW(old, hwnd, msg, wp, lp);
}

void on_test(HWND hwnd) {
    std::string host = utf8(GetDlgItem(hwnd, IDC_ED_IP));
    std::string port_s = utf8(GetDlgItem(hwnd, IDC_ED_PORT));
    std::string key = utf8(GetDlgItem(hwnd, IDC_ED_KEY));
    std::string name = utf8(GetDlgItem(hwnd, IDC_ED_NAME));

    if (host.empty() || !valid_port(port_s)) {
        set_status(hwnd, L"请先填写有效的服务器地址和端口");
        return;
    }
    set_status(hwnd, L"正在连接 " + wide(host) + L":" + wide(port_s) +
                     L" 并测试注册，请稍候……");
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_TEST), FALSE);
    UpdateWindow(hwnd);

    std::wstring result = test_register(host, std::stoi(port_s), key, name);

    EnableWindow(GetDlgItem(hwnd, IDC_BTN_TEST), TRUE);
    set_status(hwnd, result);
}

void on_save(HWND hwnd) {
    DlgData* d = data_of(hwnd);
    std::string host = utf8(GetDlgItem(hwnd, IDC_ED_IP));
    std::string port_s = utf8(GetDlgItem(hwnd, IDC_ED_PORT));

    if (host.empty()) {
        set_status(hwnd, L"服务器地址不能为空");
        return;
    }
    if (!valid_port(port_s)) {
        set_status(hwnd, L"端口必须是 1-65535 的数字");
        return;
    }
    if (!d || !d->cfg) return;
    ConfigUi* cfg = d->cfg;

    cfg->server_ip = host;
    cfg->server_port = std::stoi(port_s);
    cfg->secret_key = utf8(GetDlgItem(hwnd, IDC_ED_KEY));
    cfg->device_name = utf8(GetDlgItem(hwnd, IDC_ED_NAME));
    cfg->control_password = utf8(GetDlgItem(hwnd, IDC_ED_PWD));

    config::ClientConfig disk;
    disk.server_ip = cfg->server_ip;
    disk.server_port = cfg->server_port;
    disk.secret_key = cfg->secret_key;
    disk.device_name = cfg->device_name;
    disk.control_password = cfg->control_password;
    if (!config::ClientConfig::save(disk, cfg->config_path)) {
        set_status(hwnd, L"保存失败：无法写入 " + wide(cfg->config_path) +
                         L"（权限不足或被占用？）");
        return;
    }
    cfg->saved = true;
    std::wstring tail;
    switch (cfg->save_mode) {
        case SaveMode::SaveAndRun:
            tail = L"\r\n\r\nagent 将在后台运行并注册到服务端。";
            break;
        case SaveMode::SaveAndApply:
            tail = L"\r\n\r\n正在以新配置重连服务端。";
            break;
        default:
            tail = L"\r\n\r\n直接运行 agent.exe 即可连接服务端。";
            break;
    }
    MessageBoxW(hwnd,
                (L"配置已保存到：" + wide(cfg->config_path) + tail).c_str(),
                L"MyRemote 配置", MB_OK | MB_ICONINFORMATION);
    DestroyWindow(hwnd);
}

void create_children(HWND hwnd, DlgData* d) {
    ConfigUi* cfg = d->cfg;
    int y = d->px(kMargin);
    make_label(d, hwnd, L"服务器地址（控制端电脑的 IP）", d->px(kMargin), y,
               d->px(kFieldW));
    y += d->px(kLabelH) + d->px(2);
    make_edit(d, hwnd, IDC_ED_IP, cfg ? wide(cfg->server_ip) : L"127.0.0.1",
              d->px(kMargin), y, false);
    y += d->px(kFieldH) + d->px(kGap);

    make_label(d, hwnd, L"服务器端口", d->px(kMargin), y, d->px(kFieldW));
    y += d->px(kLabelH) + d->px(2);
    make_edit(d, hwnd, IDC_ED_PORT,
              std::to_wstring(cfg ? cfg->server_port : 7500), d->px(kMargin), y,
              false);
    y += d->px(kFieldH) + d->px(kGap);

    make_label(d, hwnd, L"连接密钥（必须与服务端 secret_key 完全一致）",
               d->px(kMargin), y, d->px(kFieldW));
    y += d->px(kLabelH) + d->px(2);
    make_edit(d, hwnd, IDC_ED_KEY, cfg ? wide(cfg->secret_key) : L"",
              d->px(kMargin), y, false);
    y += d->px(kFieldH) + d->px(kGap);

    make_label(d, hwnd, L"设备名称（留空自动使用计算机名）", d->px(kMargin), y,
               d->px(kFieldW));
    y += d->px(kLabelH) + d->px(2);
    make_edit(d, hwnd, IDC_ED_NAME, cfg ? wide(cfg->device_name) : L"",
              d->px(kMargin), y, false);
    y += d->px(kFieldH) + d->px(kGap);

    make_label(d, hwnd, L"控制密码（远程操控前的二次验证，可留空）",
               d->px(kMargin), y, d->px(kFieldW));
    y += d->px(kLabelH) + d->px(2);
    HWND pwd = make_edit(d, hwnd, IDC_ED_PWD,
                         cfg ? wide(cfg->control_password) : L"",
                         d->px(kMargin), y, true);
    WNDPROC old = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        pwd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pwd_edit_proc)));
    SetWindowLongPtrW(pwd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(old));
    y += d->px(kFieldH) + d->px(kGap) + d->px(4);

    make_button(d, hwnd, IDC_BTN_TEST, L"测试连接并注册", d->px(kMargin), y,
                140);
    y += d->px(30) + d->px(kGap);

    HWND status = CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, d->px(kMargin), y,
        d->px(kFieldW), d->px(66), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ST_STATUS)), nullptr,
        nullptr);
    SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(d->font), TRUE);
    y += d->px(66) + d->px(kGap);

    const wchar_t* save_text = L"保存";
    int save_w = 90;
    if (cfg && cfg->save_mode == SaveMode::SaveAndRun) {
        save_text = L"保存并后台运行";
        save_w = 140;
    } else if (cfg && cfg->save_mode == SaveMode::SaveAndApply) {
        save_text = L"保存并应用";
        save_w = 110;
    }
    make_button(d, hwnd, IDC_BTN_SAVE, save_text, d->px(kMargin), y, save_w);
    make_button(d, hwnd, IDC_BTN_CANCEL, L"取消",
                d->px(kMargin) + d->px(save_w + 10), y, 90);
    y += d->px(30) + d->px(kGap) + d->px(2);

    HWND file_label = CreateWindowExW(
        0, L"STATIC",
        (L"配置文件：" + (cfg ? wide(cfg->config_path) : std::wstring()))
            .c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS, d->px(kMargin), y,
        d->px(kFieldW), d->px(kLabelH), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ST_FILE)), nullptr,
        nullptr);
    SendMessageW(file_label, WM_SETFONT, reinterpret_cast<WPARAM>(d->font),
                 TRUE);

    set_status(hwnd, L"填写服务器地址后，可先点“测试连接并注册”验证。");
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    DlgData* d = data_of(hwnd);
    switch (msg) {
        case WM_CREATE: {
            if (!d) return -1;
            NONCLIENTMETRICSW ncm{};
            ncm.cbSize = sizeof(ncm);
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            d->font = CreateFontIndirectW(&ncm.lfMessageFont);
            create_children(hwnd, d);
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_BTN_TEST:
                    on_test(hwnd);
                    return 0;
                case IDC_BTN_SAVE:
                    on_save(hwnd);
                    return 0;
                case IDC_BTN_CANCEL:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (d && d->font) {
                DeleteObject(d->font);
                d->font = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void register_wndclass_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kWndClass;
        RegisterClassW(&wc);
    });
}

}  // namespace

bool run_config_gui(ConfigUi& cfg) {
    register_wndclass_once();

    DlgData data;
    data.cfg = &cfg;
    data.scale = GetDpiForSystem() / 96.0f;

    RECT rc{0, 0, data.px(kClientW), data.px(kClientH)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, 0);
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;
    HWND hwnd = CreateWindowExW(
        0, kWndClass, L"MyRemote 被控端配置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (screen_w - win_w) / 2, (screen_h - win_h) / 2, win_w, win_h,
        nullptr, nullptr, GetModuleHandleW(nullptr), &data);
    if (!hwnd) {
        return false;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    // A process started with STARTUPINFO.wShowWindow=SW_HIDE ignores its first
    // ShowWindow call, which would leave this dialog invisible forever.
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return cfg.saved;
}

void show_config_gui_async(ConfigUi cfg,
                           std::function<void(const ConfigUi&)> on_closed) {
    std::thread([cfg, on_closed]() mutable {
        bool saved = run_config_gui(cfg);
        cfg.saved = saved;
        if (on_closed) {
            on_closed(cfg);
        }
    }).detach();
}

}  // namespace gui
