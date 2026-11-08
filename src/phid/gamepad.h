#pragma once

#include "device.h"
#include "hid_codes.h"

namespace phid {

struct GamepadLayout {
    uint8_t buttonCount = 16; // 1..32
    bool hasHat = true;
    uint8_t stickCount = 2;   // 0..2: left X/Y, right Z/Rz
    bool hasTriggers = true;  // left Brake, right Accelerator (Android/Linux convention)

    GamepadLayout& buttons(uint8_t n) {
        buttonCount = n;
        return *this;
    }
    GamepadLayout& hat(bool on = true) {
        hasHat = on;
        return *this;
    }
    GamepadLayout& sticks(uint8_t n) {
        stickCount = n;
        return *this;
    }
    GamepadLayout& triggers(bool on = true) {
        hasTriggers = on;
        return *this;
    }
    bool valid() const {
        return buttonCount <= 32 && stickCount <= 2;
    }
};

// Gamepad that sends its latest state. If a button goes down and back up (or
// up and back down) between two reports, the in-between state is sent too, so
// quick taps always register. The state survives reconnects and is restated
// to the host when the link becomes ready.
class BTGamepad : public HidDevice {
public:
    explicit BTGamepad(const GamepadLayout& layout = GamepadLayout())
            : layout_(layout) {
    }

    bool press(uint8_t button) {
        return setButton(button, true);
    }
    bool release(uint8_t button) {
        return setButton(button, false);
    }
    bool button(uint8_t button, bool down) {
        return setButton(button, down);
    }
    void releaseAll();
    void hat(Hat h);
    void stick(Stick s, int16_t x, int16_t y);
    void stick(Stick s, float x, float y); // -1..1
    void trigger(Trigger t, uint8_t value);
    void trigger(Trigger t, float value); // 0..1

    const GamepadLayout& layout() const {
        return layout_;
    }

    // HidDevice
    void appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const override;
    uint8_t inputLength() const override;
    uint16_t appearance() const override {
        return 0x03C4; // HID Gamepad
    }
    bool pending(uint32_t* seq) const override;
    size_t buildReport(uint8_t* out, bool boot) override;
    void commit() override;
    void expire(uint32_t now, uint32_t maxAgeMs, bool dropAll) override {
        (void)now;
        (void)maxAgeMs;
        (void)dropAll;
    }

private:
    bool setButton(uint8_t button, bool down);
    void dirty();

    GamepadLayout layout_;
    uint32_t buttons_ = 0;      // current
    uint32_t sent_ = 0;         // last report the host accepted
    uint32_t intermediate_ = 0; // buttons that toggled away from sent_ and back
    uint32_t built_ = 0;        // buttons in the last built report
    Hat hat_ = Hat::Centered;
    int16_t axes_[4] = {};
    uint8_t triggers_[2] = {};
    bool dirty_ = false;
    uint32_t seq_ = 0;
};

} // namespace phid
