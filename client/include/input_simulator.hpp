#pragma once

#include <windows.h>

#include <cstdint>

#include "messages.hpp"

// Injects mouse/keyboard events from the controller into this session.
class InputSimulator {
public:
    InputSimulator() = default;

    void handle(const proto::InputEvent& event);

private:
    void move(int x, int y);
    void button(uint8_t button, bool pressed);
    void wheel(int delta);
    void key(uint16_t vk, bool pressed, bool extended);
    uint16_t last_vk_ = 0;
};
