#pragma once

#include <cstdint>

namespace phid {

// Maps a character to the key and modifiers that type it.
struct KeyStroke {
    uint8_t key;  // keyboard usage; 0 if the character can't be typed
    uint8_t mods; // Mod:: bits
};

class KeyboardLayout {
public:
    virtual ~KeyboardLayout() = default;
    virtual KeyStroke map(char c) const = 0;
};

// US ANSI layout: printable ASCII plus \n (Enter), \t and \b. \r is skipped so
// println() types a single Enter.
class UsLayout : public KeyboardLayout {
public:
    KeyStroke map(char c) const override;
};

extern const UsLayout layoutUs;

} // namespace phid
