#include "input_simulator.hpp"

InputSimulator::InputSimulator() {}

InputSimulator::~InputSimulator() {}

uint32_t InputSimulator::vk_to_scancode(int32_t vk) {
    return MapVirtualKey(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
}

void InputSimulator::simulate_mouse(const mouse::Event& event) {
    INPUT inputs[2];
    ZeroMemory(inputs, sizeof(INPUT));
    
    DWORD flags = 0;
    bool pressed = event.pressed;
    
    switch (event.button) {
        case LEFT_BUTTON:
            flags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
            
        case RIGHT_BUTTON:
            flags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
            
        case MIDDLE_BUTTON:
            flags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
            
        default:
            return;  // Unsupported button
    }
    
    if (flags != 0) {
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = flags;
        
        SendInput(1, inputs, sizeof(INPUT));
        
        if (pressed) {
            Sleep(50);  // Simulate button hold time
            
            // Release immediately after press
            ZeroMemory(inputs, sizeof(INPUT));
            inputs[0].type = INPUT_MOUSE;
            
            switch (event.button) {
                case LEFT_BUTTON:
                    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    break;
                case RIGHT_BUTTON:
                    inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                    break;
                case MIDDLE_BUTTON:
                    inputs[0].mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
                    break;
                default:
                    return;
            }
            
            SendInput(1, inputs, sizeof(INPUT));
        }
    }
    
    // Handle cursor movement if coordinates changed
    if (cursor_x_prev_ != event.cursor_x || cursor_y_prev_ != event.cursor_y) {
        ZeroMemory(inputs, sizeof(INPUT));
        
        INPUT move_input{};
        move_input.type = INPUT_MOUSE;
        move_input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        
        // Convert absolute position (screen coords) to 0-65535 range
        int screen_width = GetSystemMetrics(SM_CXSCREEN);
        int screen_height = GetSystemMetrics(SM_CYSCREEN);
        
        LONG mx = static_cast<LONG>(static_cast<double>(event.cursor_x) / screen_width * 65535);
        LONG my = static_cast<LONG>(static_cast<double>(event.cursor_y) / screen_height * 65535);
        
        move_input.mi.dx = mx;
        move_input.mi.dy = my;
        
        SendInput(1, &move_input, sizeof(INPUT));
        
        cursor_x_prev_ = event.cursor_x;
        cursor_y_prev_ = event.cursor_y;
    }
}

void InputSimulator::simulate_keyboard(const keyboard::Event& event) {
    INPUT inputs[1];
    ZeroMemory(inputs, sizeof(INPUT));
    
    UINT scan_code = vk_to_scancode(event.virtual_key);
    bool extended = event.extended;
    
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = static_cast<WORD>(event.virtual_key);
    inputs[0].ki.wScan = scan_code;
    inputs[0].ki.dwFlags = 0;
    
    if (event.type == KEY_RELEASE) {
        inputs[0].ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    
    if (extended) {
        inputs[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    
    SendInput(1, inputs, sizeof(INPUT));
}
