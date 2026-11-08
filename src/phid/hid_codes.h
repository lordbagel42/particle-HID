#pragma once

#include <cstdint>

namespace phid {

// Keyboard/Keypad usage page (0x07).
enum class Key : uint8_t {
    None = 0x00,
    A = 0x04, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num1 = 0x1E, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,
    Enter = 0x28,
    Escape = 0x29,
    Backspace = 0x2A,
    Tab = 0x2B,
    Space = 0x2C,
    Minus = 0x2D,
    Equal = 0x2E,
    LeftBracket = 0x2F,
    RightBracket = 0x30,
    Backslash = 0x31,
    NonUsHash = 0x32,
    Semicolon = 0x33,
    Quote = 0x34,
    Grave = 0x35,
    Comma = 0x36,
    Period = 0x37,
    Slash = 0x38,
    CapsLock = 0x39,
    F1 = 0x3A, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    PrintScreen = 0x46,
    ScrollLock = 0x47,
    Pause = 0x48,
    Insert = 0x49,
    Home = 0x4A,
    PageUp = 0x4B,
    Delete = 0x4C,
    End = 0x4D,
    PageDown = 0x4E,
    Right = 0x4F,
    Left = 0x50,
    Down = 0x51,
    Up = 0x52,
    NumLock = 0x53,
    KeypadSlash = 0x54,
    KeypadAsterisk = 0x55,
    KeypadMinus = 0x56,
    KeypadPlus = 0x57,
    KeypadEnter = 0x58,
    Keypad1 = 0x59, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9, Keypad0,
    KeypadDot = 0x63,
    NonUsBackslash = 0x64,
    Application = 0x65,
    F13 = 0x68, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,
    LeftCtrl = 0xE0,
    LeftShift = 0xE1,
    LeftAlt = 0xE2,
    LeftGui = 0xE3,
    RightCtrl = 0xE4,
    RightShift = 0xE5,
    RightAlt = 0xE6,
    RightGui = 0xE7,
};

// Modifier bits for BTKeyboard::tap() and the first byte of a keyboard report.
namespace Mod {
constexpr uint8_t None = 0x00;
constexpr uint8_t LeftCtrl = 0x01;
constexpr uint8_t LeftShift = 0x02;
constexpr uint8_t LeftAlt = 0x04;
constexpr uint8_t LeftGui = 0x08;
constexpr uint8_t RightCtrl = 0x10;
constexpr uint8_t RightShift = 0x20;
constexpr uint8_t RightAlt = 0x40;
constexpr uint8_t RightGui = 0x80;
} // namespace Mod

// Keyboard LED bits delivered by the host in the output report.
namespace Led {
constexpr uint8_t NumLock = 0x01;
constexpr uint8_t CapsLock = 0x02;
constexpr uint8_t ScrollLock = 0x04;
constexpr uint8_t Compose = 0x08;
constexpr uint8_t Kana = 0x10;
} // namespace Led

// Consumer usage page (0x0C).
enum class Media : uint16_t {
    None = 0x000,
    BrightnessUp = 0x06F,
    BrightnessDown = 0x070,
    NextTrack = 0x0B5,
    PreviousTrack = 0x0B6,
    Stop = 0x0B7,
    Eject = 0x0B8,
    PlayPause = 0x0CD,
    Mute = 0x0E2,
    VolumeUp = 0x0E9,
    VolumeDown = 0x0EA,
    Email = 0x18A,
    Calculator = 0x192,
    FileBrowser = 0x194,
    Search = 0x221,
    Home = 0x223,
    Back = 0x224,
    Forward = 0x225,
    Refresh = 0x227,
};

// Generic Desktop system controls (0x01, 0x81..0x83).
enum class SystemKey : uint8_t {
    None = 0x00,
    PowerDown = 0x81,
    Sleep = 0x82,
    WakeUp = 0x83,
};

namespace MouseButton {
constexpr uint8_t Left = 0x01;
constexpr uint8_t Right = 0x02;
constexpr uint8_t Middle = 0x04;
constexpr uint8_t Back = 0x08;
constexpr uint8_t Forward = 0x10;
} // namespace MouseButton

// Gamepad button numbers (1-based HID button usages). The names follow the order
// Linux and Android map for a Game Pad collection (BTN_SOUTH, BTN_EAST, ...).
namespace Button {
constexpr uint8_t A = 1;
constexpr uint8_t B = 2;
constexpr uint8_t C = 3;
constexpr uint8_t X = 4;
constexpr uint8_t Y = 5;
constexpr uint8_t Z = 6;
constexpr uint8_t L1 = 7;
constexpr uint8_t R1 = 8;
constexpr uint8_t L2 = 9;
constexpr uint8_t R2 = 10;
constexpr uint8_t Select = 11;
constexpr uint8_t Start = 12;
constexpr uint8_t Home = 13;
constexpr uint8_t L3 = 14;
constexpr uint8_t R3 = 15;
} // namespace Button

enum class Hat : uint8_t {
    Up = 0,
    UpRight = 1,
    Right = 2,
    DownRight = 3,
    Down = 4,
    DownLeft = 5,
    Left = 6,
    UpLeft = 7,
    Centered = 8,
};

enum class Stick : uint8_t { Left = 0, Right = 1 };
enum class Trigger : uint8_t { Left = 0, Right = 1 };

} // namespace phid
