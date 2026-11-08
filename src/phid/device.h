#pragma once

#include <cstddef>
#include <cstdint>

#include "report_map.h"

namespace phid {

enum class BootRole : uint8_t { None, BootKeyboard, BootMouse };

struct DeviceStats {
    uint32_t sent = 0;              // reports the host accepted
    uint32_t rejectedFull = 0;      // calls refused because the queue was full
    uint32_t rejectedInvalid = 0;   // unknown key, 7th simultaneous key, bad button
    uint32_t droppedNotReady = 0;   // events dropped after waiting too long for the host
};

// Base class for everything that produces HID input reports.
//
// Every report a device sends restates its whole state, so the host can never
// drift. Event devices (keyboard, mouse buttons, media keys) keep an ordered
// queue and send one report per transition. State devices (gamepad) send the
// newest state. All delivery methods run with the library lock held.
class HidDevice {
public:
    virtual ~HidDevice() = default;

    // Profile.
    virtual void appendDescriptor(ReportMapBuilder& b, uint8_t reportId) const = 0;
    virtual uint8_t inputLength() const = 0; // bytes, without the Report ID
    virtual uint8_t outputLength() const {
        return 0;
    }
    virtual BootRole bootRole() const {
        return BootRole::None;
    }
    virtual uint16_t appearance() const = 0;

    // Delivery.
    // True when a report is waiting; *seq orders it against other devices.
    virtual bool pending(uint32_t* seq) const = 0;
    // Writes the next report without consuming it. commit() consumes it after
    // the host accepted it, so a busy radio never loses a report.
    virtual size_t buildReport(uint8_t* out, bool boot) = 0;
    virtual void commit() = 0;
    // Drops queued events older than maxAgeMs (all events when dropAll) while
    // the host can't receive them. Held keys and buttons stay held.
    virtual void expire(uint32_t now, uint32_t maxAgeMs, bool dropAll) = 0;
    // The next report restates the current state even when nothing changed.
    virtual void markSync();
    virtual void onOutput(const uint8_t* data, size_t len) {
        (void)data;
        (void)len;
    }
    // Runs user callbacks on the HID worker, without the library lock.
    virtual void runCallbacks() {
    }

    const DeviceStats& stats() const {
        return stats_;
    }
    uint8_t reportId() const {
        return reportId_;
    }

protected:
    struct Stamp {
        uint32_t seq;
        uint32_t time;
    };
    static Stamp stamp();
    static bool before(uint32_t a, uint32_t b) {
        return (int32_t)(a - b) < 0;
    }
    bool hostReady() const;
    void wake() const;

    uint8_t reportId_ = 0;
    bool sync_ = false;
    uint32_t syncSeq_ = 0;
    DeviceStats stats_;
    const volatile bool* ready_ = nullptr;

    friend class HidEngine;
};

} // namespace phid
