#include "input_simulator.hpp"

namespace {
WORD button_flag(uint8_t button, bool pressed) {
    switch (button) {
        case 0: return pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        case 1: return pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        case 2: return pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        default: return 0;
    }
}
}  // namespace

void InputSimulator::handle(const proto::InputEvent& event) {
    switch (event.kind) {
        case proto::InputKind::MouseMove:
            move(event.x, event.y);
            break;
        case proto::InputKind::MouseButton:
            button(event.button, event.pressed);
            break;
        case proto::InputKind::MouseWheel:
            wheel(event.delta);
            break;
        case proto::InputKind::Key:
            key(event.vk, event.pressed, event.extended);
            break;
        default:
            break;
    }
}

void InputSimulator::move(int x, int y) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0) sw = 1;
    if (sh <= 0) sh = 1;

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                    MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = static_cast<LONG>((x * 65535) / sw);
    in.mi.dy = static_cast<LONG>((y * 65535) / sh);
    SendInput(1, &in, sizeof(INPUT));
}

void InputSimulator::button(uint8_t button, bool pressed) {
    DWORD flag = button_flag(button, pressed);
    if (flag == 0) return;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    SendInput(1, &in, sizeof(INPUT));
}

void InputSimulator::wheel(int delta) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(delta);
    SendInput(1, &in, sizeof(INPUT));
}

void InputSimulator::key(uint16_t vk, bool pressed, bool extended) {
    last_vk_ = vk;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    if (extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!pressed) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}
