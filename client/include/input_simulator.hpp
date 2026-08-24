#pragma once

#include <windows.h>

// Mouse button enumeration matching protocol definition
namespace mouse {
    enum Button {
        LEFT_BUTTON = 0,
        RIGHT_BUTTON = 1,
        MIDDLE_BUTTON = 2,
        X_BUTTON_1 = 3,
        X_BUTTON_2 = 4
    };
    
    struct Event {
        int32_t cursor_x;
        int32_t cursor_y;
        Button button;
        bool pressed;
        bool double_click;
    };
}

// Keyboard event structure
namespace keyboard {
    enum Type {
        KEY_PRESS = 0,
        KEY_RELEASE = 1
    };
    
    struct Event {
        Type type;
        int32_t virtual_key;     // VK_CODE (e.g., VK_A, VK_F4)
        uint32_t scan_code;      // Physical scancode
        bool extended;           // Extended key flag
    };
}

// Input simulator for remote control
class InputSimulator {
public:
    InputSimulator();
    ~InputSimulator();
    
    // Simulate mouse movement and button events
    void simulate_mouse(const mouse::Event& event);
    
    // Simulate keyboard press/release
    void simulate_keyboard(const keyboard::Event& event);
    
private:
    // Convert virtual key to scancode
    static uint32_t vk_to_scancode(int32_t vk);
};
