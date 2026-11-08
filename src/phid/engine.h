#pragma once

#include <cstddef>
#include <cstdint>

#include "descriptor_parser.h"
#include "device.h"

namespace phid {

enum class SendResult : uint8_t {
    Ok,            // the radio accepted the report
    Busy,          // transmit queue full; retry after a TX-complete wake
    NotSubscribed, // the host hasn't enabled notifications for this report
    Error,
};

// Where reports go. On a device this is the BLE GATT server.
class Transport {
public:
    virtual ~Transport() = default;
    virtual SendResult sendInput(size_t deviceIndex, bool boot, const uint8_t* data, size_t len) = 0;
};

enum class ProfileError : uint8_t {
    None = 0,
    NoDevices,
    TooManyDevices,
    DuplicateBootRole, // two keyboards or two mice can't share the boot reports
    InvalidLayout,
    ReportMapTooLarge,
    DescriptorInvalid, // the composed Report Map fails to parse
    LengthMismatch,    // a device's report length disagrees with its descriptor
};

struct EngineStats {
    uint32_t sent = 0;
    uint32_t busy = 0;
    uint32_t sendErrors = 0;
};

// Composes the HID profile from devices and moves their reports to the host in
// the order they happened, across all devices.
class HidEngine {
public:
    static constexpr size_t kMaxDevices = 8;
    static constexpr size_t kMaxReportMap = 512;
    static constexpr size_t kMaxReport = 32;

    // Before buildProfile() only.
    bool add(HidDevice& device);
    ProfileError buildProfile();

    size_t deviceCount() const {
        return count_;
    }
    HidDevice& device(size_t i) {
        return *devices_[i];
    }
    const uint8_t* reportMap() const {
        return map_;
    }
    size_t reportMapLength() const {
        return mapLen_;
    }
    const DescriptorSummary& summary() const {
        return summary_;
    }
    // Index of the device with this boot role, or -1.
    int bootDevice(BootRole role) const;

    // Link state, called with the library lock held.
    void setReady(bool ready);
    bool ready() const {
        return ready_;
    }
    void setSubscribed(size_t i, bool subscribed) {
        subscribed_[i] = subscribed;
    }
    bool subscribed(size_t i) const {
        return subscribed_[i];
    }
    void setBootProtocol(bool boot) {
        boot_ = boot;
    }
    bool bootProtocol() const {
        return boot_;
    }
    void readyDeadline(uint32_t ms) {
        readyDeadlineMs_ = ms;
    }

    // Expires what can't be delivered and sends what can. Lock held.
    void service(uint32_t now, Transport& transport);
    // Drops every queued event, keeping held state (on disconnect). Lock held.
    void dropQueued(uint32_t now);
    bool anyPending() const;

    const EngineStats& stats() const {
        return stats_;
    }

private:
    bool deliverable(size_t i) const;

    HidDevice* devices_[kMaxDevices] = {};
    size_t count_ = 0;
    bool built_ = false;
    uint8_t map_[kMaxReportMap] = {};
    size_t mapLen_ = 0;
    DescriptorSummary summary_;
    volatile bool ready_ = false;
    bool subscribed_[kMaxDevices] = {};
    bool boot_ = false;
    uint32_t readyDeadlineMs_ = 2000;
    EngineStats stats_;
};

} // namespace phid
