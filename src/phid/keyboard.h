#pragma once

#include "device.h"
#include "hid_codes.h"
#include "keyboard_layout.h"
#include "ring.h"

#if defined(PARTICLE)
#include "spark_wiring_print.h"
#endif

namespace phid {

// 6-key-rollover keyboard with LED output and boot protocol support.
//
// Every press and release reaches the host in order, one report each. A
// release is never refused: the queue always keeps room to release every key
// that is down, so keys can't get stuck on the host.
class BTKeyboard : public HidDevice
#if defined(PARTICLE)
        ,
                 public Print
#endif
{
public:
    static constexpr size_t kQueueSize = 64;
    static constexpr uint8_t kMaxKeyUsage = 0xA4;

    explicit BTKeyboard(const KeyboardLayout& layout = layoutUs)
            : layout_(&layout) {
    }

    bool press(Key key) {
        return press((uint8_t)key);
    }
    bool press(uint8_t usage);
    bool release(Key key) {
        return release((uint8_t)key);
    }
    bool release(uint8_t usage);
    void releaseAll();
    // Press and release, holding the given Mod:: bits around it.
    bool tap(Key key, uint8_t mods = Mod::None);
    // Types text with the layout; returns how many characters were queued.
    size_t type(const char* text);

    // Print support (print, println, printf). Waits up to writeTimeout() for
    // queue space while the host is connected.
#if defined(PARTICLE)
    size_t write(uint8_t c) override;
    using Print::write;
#else
    size_t write(uint8_t c);
#endif
    void writeTimeout(uint32_t ms) {
        writeTimeoutMs_ = ms;
    }
    uint32_t writeTimeout() const {
        return writeTimeoutMs_;
    }

    void layout(const KeyboardLayout& l) {
        layout_ = &l;
    }

    // Host LED state (Led:: bits) and a callback run on the HID worker when it changes.
    uint8_t leds() const {
        return leds_;
    }
    void onLeds(void (*callback)(uint8_t leds)) {
        ledCallback_ = callback;
    }

    // HidDevice
    void appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const override;
    uint8_t inputLength() const override {
        return 8;
    }
    uint8_t outputLength() const override {
        return 1;
    }
    BootRole bootRole() const override {
        return BootRole::BootKeyboard;
    }
    uint16_t appearance() const override {
        return 0x03C1; // HID Keyboard
    }
    bool pending(uint32_t* seq) const override;
    size_t buildReport(uint8_t* out, bool boot) override;
    void commit() override;
    void expire(uint32_t now, uint32_t maxAgeMs, bool dropAll) override;
    void onOutput(const uint8_t* data, size_t len) override;
    void runCallbacks() override;

private:
    struct State {
        uint8_t mods;
        uint8_t keys[6];
        bool has(uint8_t usage) const;
        bool add(uint8_t usage);
        void remove(uint8_t usage);
        size_t down() const;
        size_t keyCount() const;
    };
    struct Event {
        uint8_t usage;
        uint8_t down;
        uint32_t seq;
        uint32_t time;
    };

    static bool isModifier(uint8_t usage) {
        return usage >= 0xE0 && usage <= 0xE7;
    }
    static bool valid(uint8_t usage) {
        return (usage >= 0x04 && usage <= kMaxKeyUsage) || isModifier(usage);
    }
    static void apply(State& s, const Event& e);
    void push(uint8_t usage, bool down);
    enum class TapResult { Ok, Invalid, Full };
    TapResult tapLocked(uint8_t usage, uint8_t mods);

    const KeyboardLayout* layout_;
    Ring<Event, kQueueSize> queue_;
    State base_ = {};    // what the host has (sent or expired events)
    State logical_ = {}; // after every queued event
    uint32_t writeTimeoutMs_ = 2000;
    volatile uint8_t leds_ = 0;
    volatile bool ledsChanged_ = false;
    void (*ledCallback_)(uint8_t) = nullptr;
};

} // namespace phid
