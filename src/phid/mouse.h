#pragma once

#include "device.h"
#include "hid_codes.h"
#include "ring.h"

namespace phid {

// Relative mouse: 5 buttons, 16-bit X/Y, wheel and horizontal pan.
//
// Motion is summed, never dropped while the host is connected: large moves go
// out in clamped chunks and the rest carries over. Button changes are ordered
// against motion, so a click lands where the pointer was when you clicked.
class BTMouse : public HidDevice {
public:
    static constexpr size_t kQueueSize = 32;

    bool move(int32_t dx, int32_t dy, int32_t wheel = 0, int32_t pan = 0);
    bool scroll(int32_t wheel, int32_t pan = 0) {
        return move(0, 0, wheel, pan);
    }
    bool press(uint8_t buttons = MouseButton::Left);
    bool release(uint8_t buttons = MouseButton::Left);
    bool click(uint8_t buttons = MouseButton::Left);
    bool isPressed(uint8_t buttons) const {
        return (logical_ & buttons) == buttons;
    }

    // HidDevice
    void appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const override;
    uint8_t inputLength() const override {
        return 7;
    }
    BootRole bootRole() const override {
        return BootRole::BootMouse;
    }
    uint16_t appearance() const override {
        return 0x03C2; // HID Mouse
    }
    bool pending(uint32_t* seq) const override;
    size_t buildReport(uint8_t* out, bool boot) override;
    void commit() override;
    void expire(uint32_t now, uint32_t maxAgeMs, bool dropAll) override;

private:
    enum Kind : uint8_t { Move, Buttons };
    struct Event {
        Kind kind;
        uint8_t buttons;
        int32_t x, y, wheel, pan;
        uint32_t seq;
        uint32_t time;
    };
    struct Chunk {
        int32_t x, y, wheel, pan;
    };

    bool setButtons(uint8_t next);

    Ring<Event, kQueueSize> queue_;
    uint8_t base_ = 0;
    uint8_t logical_ = 0;
    Chunk last_ = {};
    bool lastBoot_ = false;
};

} // namespace phid
