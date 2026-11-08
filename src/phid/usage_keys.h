#pragma once

#include "device.h"
#include "hid_codes.h"
#include "ring.h"

namespace phid {

// One-usage-at-a-time keys (media keys, system power keys). Pressing a second
// key while one is held replaces it, the way a single-slot HID array works.
class UsageKeys : public HidDevice {
public:
    static constexpr size_t kQueueSize = 16;

    bool pending(uint32_t* seq) const override;
    size_t buildReport(uint8_t* out, bool boot) override;
    void commit() override;
    void expire(uint32_t now, uint32_t maxAgeMs, bool dropAll) override;

protected:
    bool pressUsage(uint16_t usage);
    bool releaseUsage(uint16_t usage);
    bool tapUsage(uint16_t usage);
    virtual void encode(uint16_t usage, uint8_t* out) const = 0;

private:
    struct Event {
        uint16_t usage; // new current usage, 0 = none
        uint32_t seq;
        uint32_t time;
    };
    void push(uint16_t usage);

    Ring<Event, kQueueSize> queue_;
    uint16_t base_ = 0;
    uint16_t logical_ = 0;
};

// Consumer controls: volume, playback, brightness, browser keys.
class BTMediaKeys : public UsageKeys {
public:
    bool press(Media m) {
        return pressUsage((uint16_t)m);
    }
    bool release(Media m) {
        return releaseUsage((uint16_t)m);
    }
    bool tap(Media m) {
        return tapUsage((uint16_t)m);
    }

    void appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const override;
    uint8_t inputLength() const override {
        return 2;
    }
    uint16_t appearance() const override {
        return 0x03C0; // generic HID
    }

protected:
    void encode(uint16_t usage, uint8_t* out) const override {
        out[0] = (uint8_t)usage;
        out[1] = (uint8_t)(usage >> 8);
    }
};

// System power down, sleep and wake.
class BTSystemControl : public UsageKeys {
public:
    bool press(SystemKey k) {
        return pressUsage((uint16_t)k);
    }
    bool release(SystemKey k) {
        return releaseUsage((uint16_t)k);
    }
    bool tap(SystemKey k) {
        return tapUsage((uint16_t)k);
    }

    void appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const override;
    uint8_t inputLength() const override {
        return 1;
    }
    uint16_t appearance() const override {
        return 0x03C0;
    }

protected:
    void encode(uint16_t usage, uint8_t* out) const override {
        out[0] = usage ? (uint8_t)(usage - 0x80) : 0; // array index 1..3; 0 is out of range = none
    }
};

} // namespace phid
