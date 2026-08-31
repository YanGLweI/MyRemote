#pragma once

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// Notification-area icon backed by a message-only window whose class name is
// "MyRemoteAgentTray". A message-only window is invisible to FindWindowW, so a
// second instance reaches it with FindWindowExW(HWND_MESSAGE, ...) and posts
// WM_SHOW_CONFIG to raise the configuration dialog.
class TrayIcon {
public:
    static constexpr UINT WM_SHOW_CONFIG = WM_APP + 1;  // cross-process trigger
    static constexpr wchar_t kWndClass[] = L"MyRemoteAgentTray";

    using Callback = std::function<void()>;

    struct Actions {
        Callback open_config;
        Callback quit;
        // Only set while the agent runs with a filtered token.
        Callback elevate;
        Callback install_autostart;
        // Drops or restores the tunnel without touching the process, the
        // service or the start type: the icon has to survive it, because the
        // same menu is the way back.
        Callback toggle_pause;
        Callback hide_icon;
        // Asked while the menu is being built, so the item can read as an
        // action rather than as a mode the user has to remember.
        std::function<bool()> paused;
        // On the secure desktop a window we create is invisible to whoever is
        // typing at the machine, so opening the editor would only look broken.
        std::function<bool()> config_pointless_now;
        // Service mode stops the service too, and the label has to say so.
        std::wstring quit_text = L"退出";
    };

    // Creates the tray window + icon on a dedicated message-loop thread.
    bool start(Actions actions);
    // Waits up to grace_ms for the message pump and then for whatever the
    // picked menu item is still doing. If the pump never comes back we delete
    // the icon ourselves before giving up on the thread: a painted icon whose
    // owner we abandoned is indistinguishable from a working one.
    void stop(DWORD grace_ms = 3000);
    void set_tooltip(const std::wstring& text);
    // Bumped by a timer on the message-loop thread, so it only advances while
    // that thread is actually pumping. Callers use it to tell a live tray from
    // a painted-and-stuck one.
    uint64_t liveness() const { return liveness_.load(); }

    // Runs fn off the message-loop thread. fn is copied, so it must not reach
    // back into this TrayIcon or into a stack frame that can end before it
    // does - the actions it runs are capture-free for that reason.
    void enqueue(Callback fn);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);
    void Run(HANDLE ready_event);
    void WorkerLoop();
    void ShowMenu();
    // NIM_ADD returns FALSE while the shell is absent (boot, explorer restart);
    // the icon then has to be re-added later or the machine looks tray-less.
    void AddIcon(HWND hwnd);
    void RemoveIcon();

    std::atomic<HWND> hwnd_{nullptr};
    NOTIFYICONDATAW nid_{};
    Actions actions_;
    std::thread thread_;
    std::thread worker_;
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::queue<Callback> queue_;
    bool worker_exit_ = false;
    std::atomic<bool> worker_busy_{false};
    HANDLE ready_event_ = nullptr;
    HANDLE pump_done_ = nullptr;
    HANDLE worker_done_ = nullptr;
    std::atomic<uint64_t> liveness_{0};
    bool icon_added_ = false;
    UINT wm_taskbar_created_ = 0;
};
