#pragma once

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Notification-area icon backed by a message-only window whose class name is
// "MyRemoteAgentTray": a second agent instance FindWindowW's it and posts
// WM_SHOW_CONFIG to raise the configuration dialog of the running instance.
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
    };

    // Creates the tray window + icon on a dedicated message-loop thread.
    bool start(Actions actions);
    void stop();
    void set_tooltip(const std::wstring& text);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);
    void Run(HANDLE ready_event);
    void ShowMenu();

    std::atomic<HWND> hwnd_{nullptr};
    NOTIFYICONDATAW nid_{};
    Actions actions_;
    std::thread thread_;
    HANDLE ready_event_ = nullptr;
    std::atomic<bool> stopped_{false};
};
