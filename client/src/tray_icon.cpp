#include "tray_icon.hpp"

#include <shellapi.h>
#include <tlhelp32.h>

#include <memory>
#include <string>

#include "log.hpp"

#pragma comment(lib, "shell32.lib")

// ID of the ICON resource compiled from client/resources/agent.rc.
constexpr int kAgentIconId = 101;
constexpr UINT WM_TRAY_CALLBACK = WM_APP + 2;
constexpr UINT WM_TRAY_SET_TIP = WM_APP + 3;
constexpr UINT WM_TRAY_STOP = WM_APP + 4;
constexpr UINT_PTR kAddIconRetryTimer = 1;
constexpr UINT_PTR kLivenessTimer = 2;
constexpr size_t kTipCapacity = 127;

namespace {

// No opinion registered means the pre-existing behaviour: show whatever callback
// the caller wired up. With one, the menu asks at the moment it is built.
bool wanted(const std::function<bool()>& predicate) {
    return !predicate || predicate();
}

}  // namespace

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
    if (wm_taskbar_created_ && msg == wm_taskbar_created_) {
        // explorer just (re)started; its notification area is empty, so an icon
        // we added earlier is gone and has to be registered again.
        icon_added_ = false;
        AddIcon(hwnd);
        return 0;
    }
    if (msg == WM_TIMER && wp == kAddIconRetryTimer) {
        if (!icon_added_) {
            AddIcon(hwnd);
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == kLivenessTimer) {
        // Nothing reads this counter here; the point is that only a thread that
        // gets back to GetMessage can bump it.
        liveness_.fetch_add(1);
        return 0;
    }
    switch (msg) {
        case WM_SHOW_CONFIG:
            if (actions_.open_config) enqueue(actions_.open_config);
            return 0;
        case WM_TRAY_CALLBACK:
            switch (LOWORD(lp)) {
                case WM_LBUTTONDBLCLK:
                    if (actions_.open_config) enqueue(actions_.open_config);
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
                // uFlags keeps whatever Run() gave it. Narrowing it to NIF_TIP
                // here would still let this MODIFY through but would leave any
                // later NIM_ADD without NIF_MESSAGE - an icon that paints and
                // never answers a right-click.
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
    wm_taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);  // idempotent: already-registered is fine

    // A hidden top-level tool window, not a message-only one. Message-only
    // windows are skipped by EnumWindows and by system broadcasts, so the
    // TaskbarCreated retry would never arrive, and a class-only FindWindowW
    // misses them too (only an exact class+title match reaches them, which is
    // how the old wake path failed to find this window). Top-level fixes all
    // three. Never shown.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWndClass, kWndTitle,
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hinst,
                                this);
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
        AddIcon(hwnd);
        SetTimer(hwnd, kLivenessTimer, 2000, nullptr);
    }
    SetEvent(ready_event);
    if (!hwnd) {
        SetEvent(pump_done_);
        return;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    KillTimer(hwnd, kLivenessTimer);
    RemoveIcon();
    DestroyWindow(hwnd);
    hwnd_.store(nullptr);
    SetEvent(pump_done_);
}

void TrayIcon::AddIcon(HWND hwnd) {
    if (Shell_NotifyIconW(NIM_ADD, &nid_)) {
        icon_added_ = true;
        KillTimer(hwnd, kAddIconRetryTimer);
        return;
    }
    // FALSE while no shell owns the notification area (pre-logon boot, or the
    // instant between explorer crashing and restarting). A one-shot add would
    // leave the machine looking tray-less until the next process start.
    SetTimer(hwnd, kAddIconRetryTimer, 1000, nullptr);
}

void TrayIcon::RemoveIcon() {
    if (icon_added_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        icon_added_ = false;
    }
}

void TrayIcon::WorkerLoop() {
    for (;;) {
        Callback fn;
        {
            std::unique_lock<std::mutex> lock(queue_mu_);
            queue_cv_.wait(lock,
                           [&] { return worker_exit_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (worker_exit_) {
                    break;
                }
                continue;
            }
            fn = std::move(queue_.front());
            queue_.pop();
        }
        worker_busy_.store(true);
        fn();
        worker_busy_.store(false);
    }
    SetEvent(worker_done_);
}

void TrayIcon::enqueue(Callback fn) {
    if (!fn) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        queue_.push(std::move(fn));
    }
    queue_cv_.notify_one();
}

bool TrayIcon::start(Actions actions) {
    actions_ = std::move(actions);
    ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pump_done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    worker_done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    worker_exit_ = false;
    worker_ = std::thread(&TrayIcon::WorkerLoop, this);
    thread_ = std::thread(&TrayIcon::Run, this, ready_event_);
    WaitForSingleObject(ready_event_, 3000);
    return hwnd_.load() != nullptr;
}

void TrayIcon::stop(DWORD grace_ms) {
    if (HWND h = hwnd_.load()) {
        PostMessageW(h, WM_TRAY_STOP, 0, 0);
    }
    // Both waits are on our own events rather than on the threads' handles:
    // std::thread's native handle is not waitable, and a pump stuck inside a
    // modal menu or a shell call has to be given up on without hanging us too.
    const bool pump_done =
        !thread_.joinable() ||
        WaitForSingleObject(pump_done_, grace_ms) == WAIT_OBJECT_0;
    if (pump_done) {
        if (thread_.joinable()) {
            thread_.join();
        }
    } else {
        // Take the icon down ourselves: a painted icon whose pump we abandoned
        // is indistinguishable from a working one.
        RemoveIcon();
        if (thread_.joinable()) {
            thread_.detach();
        }
    }
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        worker_exit_ = true;
    }
    queue_cv_.notify_all();
    const DWORD worker_grace = worker_busy_.load() ? grace_ms : 200;
    const bool worker_done =
        !worker_.joinable() ||
        WaitForSingleObject(worker_done_, worker_grace) == WAIT_OBJECT_0;
    if (worker_done) {
        if (worker_.joinable()) {
            worker_.join();
        }
    } else if (worker_.joinable()) {
        // Queued actions are capture-free, so a worker that outlives us can
        // only finish what it started; it cannot reach back into this object.
        worker_.detach();
    }
    if (ready_event_) {
        CloseHandle(ready_event_);
        ready_event_ = nullptr;
    }
    // An abandoned thread may still signal these, so they stay open until both
    // threads really came back. This happens at most once per bad exit.
    if (pump_done && worker_done) {
        if (pump_done_) {
            CloseHandle(pump_done_);
            pump_done_ = nullptr;
        }
        if (worker_done_) {
            CloseHandle(worker_done_);
            worker_done_ = nullptr;
        }
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
    constexpr int kCmdConfig = 1;
    constexpr int kCmdElevate = 2;
    constexpr int kCmdAutostart = 3;
    constexpr int kCmdQuit = 4;
    constexpr int kCmdPause = 5;
    constexpr int kCmdHide = 6;
    constexpr int kCmdStartService = 7;
    // A window opened here would land on the Default desktop while the person
    // at the machine is looking at Winlogon: gray it rather than lie.
    const bool config_grayed =
        actions_.config_pointless_now && actions_.config_pointless_now();
    AppendMenuW(menu, MF_STRING | (config_grayed ? MF_GRAYED : 0), kCmdConfig,
                L"打开配置");
    if (actions_.toggle_pause) {
        const bool paused = actions_.paused && actions_.paused();
        AppendMenuW(menu, MF_STRING, kCmdPause,
                    paused ? L"启动远程控制" : L"停止远程控制（本机不再被远控）");
    }
    if (actions_.elevate && wanted(actions_.show_autostart_group)) {
        AppendMenuW(menu, MF_STRING, kCmdElevate,
                    L"以管理员身份重启（控制提权窗口）");
    }
    if (actions_.install_autostart && wanted(actions_.show_autostart_group)) {
        AppendMenuW(menu, MF_STRING, kCmdAutostart,
                    L"安装开机自启（管理员权限）");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    bool service_item = false;
    if (actions_.start_service && wanted(actions_.show_start_service)) {
        service_item = true;
        // Unset means "no opinion registered", which keeps every caller that
        // predates this working verbatim.
        const std::wstring label =
            actions_.start_service_text
                ? actions_.start_service_text()
                : std::wstring(L"启用后台服务（当前：前台运行，开机不自启）");
        AppendMenuW(menu, MF_STRING, kCmdStartService, label.c_str());
    }
    if (actions_.hide_icon) {
        AppendMenuW(menu, MF_STRING, kCmdHide, L"隐藏托盘图标");
    }
    if (actions_.quit) {
        AppendMenuW(menu, MF_STRING, kCmdQuit, actions_.quit_text.c_str());
    }
    // No outside process can enumerate this popup: UIAutomation, MSAA and
    // MN_GETHMENU all come back empty for a #32768 owned by another process, so
    // the only reading of what was actually drawn is the one taken here.
    mlog::info(std::string("Tray menu built: ") +
               std::to_string(GetMenuItemCount(menu)) +
               " entries, service item " + (service_item ? "present" : "absent"));
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x,
                               pt.y, h, nullptr);
    DestroyMenu(menu);
    PostMessageW(h, WM_NULL, 0, 0);
    // Everything a picked item does can block - a UAC prompt, schtasks, a pipe
    // whose peer stopped reading. This loop is the only thing that can answer
    // the next right-click, so the work goes to the worker thread instead.
    Callback action;
    switch (cmd) {
        case kCmdConfig:
            action = actions_.open_config;
            break;
        case kCmdPause:
            action = actions_.toggle_pause;
            break;
        case kCmdHide:
            action = actions_.hide_icon;
            break;
        case kCmdStartService:
            action = actions_.start_service;
            break;
        case kCmdElevate:
            action = actions_.elevate;
            break;
        case kCmdAutostart:
            action = actions_.install_autostart;
            break;
        case kCmdQuit:
            action = actions_.quit;
            break;
        default:
            break;
    }
    enqueue(std::move(action));
}
