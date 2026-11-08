#pragma once

#include <cstddef>
#include <cstdint>

#include "device.h"
#include "engine.h"

namespace phid {

enum class HidState : uint8_t {
    Off,         // begin() not called, or end()
    Advertising, // waiting for a host
    Connected,   // a host is connected but can't receive reports yet (pairing, encrypting, subscribing)
    Ready,       // reports are flowing
    Error,       // see HidStatus::reason
};

enum class HidReason : uint8_t {
    None,
    NeedsPatchedDeviceOS, // the system part lacks the PHID ABI; flash the patched system-part1
    UnsupportedPlatform,  // not an nRF52840 device
    ProfileInvalid,       // see BleHidClass::profileError()
    GattFailed,           // the SoftDevice refused a service (attribute table full?)
    BleStackFailed,
    AlreadyStarted,
    InstallingPatch,      // installing the bundled patched system part; the device resets shortly
};

enum class ConnectionPreset : uint8_t {
    LowLatency, // 11.25-15 ms, no peripheral latency
    Balanced,   // 15-30 ms, latency 4 (default)
    LowPower,   // 30-60 ms, latency 10
};

enum class StatusLed : uint8_t {
    None,
    Rgb, // short overlays on the RGB status LED (default)
};

struct HidStatus {
    HidState state = HidState::Off;
    HidReason reason = HidReason::None;
    int8_t bond = -1;        // bond slot of the connected host, -1 if none
    uint8_t security = 0;    // link security level, 1 (open) to 4
    bool pairing = false;    // pairing window open
    bool bootProtocol = false;
    uint8_t bondCount = 0;
};

struct BleHidStats {
    uint32_t connections = 0;
    uint32_t disconnects = 0;
    uint8_t lastDisconnectReason = 0; // HCI status code
    uint32_t bondedReconnects = 0;
    uint32_t newBonds = 0;
    uint32_t pairingRejected = 0;     // attempts outside the pairing window
    uint32_t securityFailures = 0;
    uint8_t lastSecurityStatus = 0;   // BLE_GAP_SEC_STATUS
    uint32_t serviceChanged = 0;      // profile changes committed to a bond
    uint32_t scConfirmed = 0;         // Service Changed indications the host confirmed
    uint32_t scLastResult = 0;        // last sd_ble_gatts_service_changed() result (0 = sent)
    uint32_t cccdWrites = 0;          // subscription changes written by the host
    uint32_t storeErrors = 0;         // bond files that failed to read or write
    uint32_t lastTimeToReadyMs = 0;   // connect to first deliverable report
    uint32_t workerOverflows = 0;
    uint32_t outputReports = 0;       // keyboard LED reports written by the host
    EngineStats reports;
};

enum class BatterySource : uint8_t {
    Auto,   // System.batteryCharge()
    Manual, // batteryLevel()
    None,   // no Battery service
};

// The BLE HID peripheral. One instance, BleHid, like BLE and Cellular.
class BleHidClass {
public:
    // Configuration, before begin().
    BleHidClass& name(const char* name);              // up to 20 bytes
    BleHidClass& manufacturer(const char* name);
    BleHidClass& model(const char* name);
    BleHidClass& appearance(uint16_t appearance);     // default: from the first device
    // PnP ID: source 1 = Bluetooth SIG, 2 = USB-IF. Default: pid.codes test VID 0x1209/0x0001.
    BleHidClass& pnpId(uint8_t source, uint16_t vid, uint16_t pid, uint16_t version);
    BleHidClass& add(HidDevice& device);
    BleHidClass& connection(ConnectionPreset preset);
    BleHidClass& maxBonds(uint8_t n);                 // 1-8, default 4
    BleHidClass& evictOldestBond(bool evict);         // when full: replace LRU (default) or refuse
    BleHidClass& autoPairWhenEmpty(bool on);          // open pairing while there are no bonds (default)
    BleHidClass& battery(BatterySource source);
    BleHidClass& readyDeadline(uint32_t ms);          // how long input waits for a host (default 2000)
    // Bluetooth status on the RGB LED (default), on a plain LED such as D7, or nowhere.
    BleHidClass& statusLed(StatusLed mode);
    BleHidClass& statusLed(uint16_t pin);
    // When the system part lacks the patch, begin() installs the patched
    // system part bundled as an asset (assetOtaDir=lib/particle-hid/assets)
    // and the device resets. Default on; does nothing without the asset.
    BleHidClass& installPatch(bool on);
    // Development only: run on stock Device OS without bonding. Most hosts
    // will not keep the device; Linux usually works.
    BleHidClass& allowUnbonded(bool on);

    // Validates the profile, builds GATT, loads bonds, starts advertising.
    // Never waits for a host. Returns 0 or a SYSTEM_ERROR_* code.
    int begin();
    void end();

    void startPairing(uint32_t durationMs = 120000);
    void stopPairing();
    bool pairing() const;

    size_t bondCount() const;
    bool forget(size_t slot);
    void forgetAll();

    void batteryLevel(uint8_t percent);

    HidStatus status() const;
    bool ready() const;
    bool waitReady(uint32_t timeoutMs);
    ProfileError profileError() const {
        return profileError_;
    }
    // True when the running system part has the PHID ABI.
    static bool deviceOsSupported();

    // Runs on the HID worker thread.
    void onStatus(void (*callback)(const HidStatus& status));

    BleHidStats stats() const;

    // Internal.
    HidEngine& engine() {
        return engine_;
    }

private:
    friend struct BleHidImpl;
    HidEngine engine_;
    ProfileError profileError_ = ProfileError::None;
};

extern BleHidClass BleHid;

} // namespace phid
