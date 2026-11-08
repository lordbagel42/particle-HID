#pragma once

#include <cstdint>

#include "ble_hid.h"

namespace phid {

// Turns HID status changes into LED patterns. Portable; the Particle side
// shows the result on the RGB LED (as a short overlay) or on a plain LED.
class StatusLight {
public:
    struct Output {
        bool show = false; // false: leave the LED to Device OS (RGB) or turn it off (pin)
        uint32_t rgb = 0;  // 0x000000 while showing means dark
        bool operator==(const Output& o) const {
            return show == o.show && rgb == o.rgb;
        }
        bool operator!=(const Output& o) const {
            return !(*this == o);
        }
    };

    // steady: also show the connection state between events (a plain LED,
    // which nothing else uses). The RGB LED only shows events and pairing.
    explicit StatusLight(bool steady = false) : steady_(steady) {
    }

    // newBonds and failures are running counters (BleHidStats); their
    // increments are the events.
    Output update(uint32_t now, const HidStatus& s, uint32_t newBonds, uint32_t failures);

    static constexpr uint32_t kBlue = 0x0000FF;
    static constexpr uint32_t kGreen = 0x00FF00;
    static constexpr uint32_t kOrange = 0xFF4000;
    static constexpr uint32_t kRed = 0xFF0000;

private:
    enum class Event : uint8_t { None, Ready, Paired, Disconnected, Failed };
    void start(Event e, uint32_t now);

    bool steady_;
    bool init_ = false;
    HidState state_ = HidState::Off;
    uint32_t newBonds_ = 0;
    uint32_t failures_ = 0;
    bool bondedThisLink_ = false; // a new bond waiting for its first Ready
    Event event_ = Event::None;
    uint32_t eventAt_ = 0;
    bool pairing_ = false;
    uint32_t pairingAt_ = 0;
};

} // namespace phid
