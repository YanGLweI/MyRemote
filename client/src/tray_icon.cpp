#include "tray_icon.hpp"

#include <shellapi.h>
#include <tlhelp32.h>

#include <memory>

#pragma comment(lib, "shell32.lib")

// ID of the ICON resource compiled from client/resources/agent.rc.
constexpr int kAgentIconId = 101;
constexpr UINT WM_TRAY_CALLBACK = WM_APP + 2;
constexpr UINT WM_TRAY_SET_TIP = WM_APP + 3;
constexpr UINT WM_TRAY_STOP = WM_APP + 4;
constexpr size_t kTipCapacity = 127;

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    TrayIcon* self = reinterpret_cast<TrayIcon*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(hwnd, msg, wp, lp)
                : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayIcon::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SHOW_CONFIG:
            if (on_open_config_) on_open_config_();
            return 0;
        case WM_TRAY_CALLBACK:
            switch (LOWORD(lp)) {
                case WM_LBUTTONDBLCLK:
                    if (on_open_config_) on_open_config_();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowMenu();
                    break;
                default:
                    break;
            }
            return 0;
        case WM_TRAY_SET_TIP: {
            std::unique_ptr<std::wstring> tip(
                reinterpret_cast<std::wstring*>(lp));
            if (tip) {
                wcsncpy_s(nid_.szTip, tip->c_str(), _TRUNCATE);
                nid_.uFlags = NIF_TIP;
                Shell_NotifyIconW(NIM_MODIFY, &nid_);
            }
            return 0;
        }
        case WM_TRAY_STOP:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayIcon::Run(HANDLE ready_event) {
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);  // idempotent: already-registered is fine

    HWND hwnd = CreateWindowExW(0, kWndClass, L"MyRemote Agent", WS_OVERLAPPED,
                                0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, this);
    if (hwnd) {
        hwnd_.store(hwnd);
        nid_ = {};
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd;
        nid_.uID = 1;
        nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid_.uCallbackMessage = WM_TRAY_CALLBACK;
        HICON icon = static_cast<HICON>(LoadImageW(
            hinst, MAKEINTRESOURCEW(kAgentIconId), IMAGE_ICON, 0, 0,
            LR_DEFAULTSIZE | LR_SHARED));
        nid_.hIcon = icon
                         ? icon
                         : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
        wcscpy_s(nid_.szTip, L"MyRemote Agent");
        Shell_NotifyIconW(NIM_ADD, &nid_);
    }
    SetEvent(ready_event);
    if (!hwnd) {
        return;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    DestroyWindow(hwnd);
    hwnd_.store(nullptr);
}

bool TrayIcon::start(Callback on_open_config, Callback on_quit) {
    on_open_config_ = std::move(on_open_config);
    on_quit_ = std::move(on_quit);
    ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread(&TrayIcon::Run, this, ready_event_);
    WaitForSingleObject(ready_event_, 3000);
    return hwnd_.load() != nullptr;
}

void TrayIcon::stop() {
    stopped_.store(true);
    if (HWND h = hwnd_.load()) {
        PostMessageW(h, WM_TRAY_STOP, 0, 0);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (ready_event_) {
        CloseHandle(ready_event_);
        ready_event_ = nullptr;
    }
}

void TrayIcon::set_tooltip(const std::wstring& text) {
    HWND h = hwnd_.load();
    if (!h) return;
    std::wstring* tip = new std::wstring(text);
    if (tip->size() > kTipCapacity) {
        tip->resize(kTipCapacity);
    }
    if (!PostMessageW(h, WM_TRAY_SET_TIP, 0,
                      reinterpret_cast<LPARAM>(tip))) {
        delete tip;
    }
}

void TrayIcon::ShowMenu() {
    HWND h = hwnd_.load();
    if (!h) return;
    SetForegroundWindow(h);  // lets TrackPopupMenu dismiss reliably
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"打开配置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"退出");
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x,
                               pt.y, h, nullptr);
    DestroyMenu(menu);
    PostMessageW(h, WM_NULL, 0, 0);
    if (cmd == 1 && on_open_config_) {
        on_open_config_();
    } else if (cmd == 2 && on_quit_) {
        on_quit_();
    }
}
