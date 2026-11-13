// HAVEN badge: a BLE keyboard + media keys whose RGB LED the paired computer
// can control over the same Bluetooth connection (tools/badge-led).
//
// The LED service is a plain Device OS BLE characteristic. It shares the one
// link the host's HID driver already holds, so no second connection is needed.
// Commands (UTF-8 text, written to the LED characteristic):
//   off | on | status | red | green | blue | white | yellow | cyan | magenta | orange | purple | pink | #rrggbb
// "status" hands the LED back to Device OS (normal breathing cyan, etc.).
// Reading the characteristic returns the current command.
//
// Power: cellular is most of the battery draw, so the cloud is off except for
// 5 minutes after boot (press RESET to flash it over the air), a short hourly
// check-in that only looks for a firmware update, and on request:
//   cloud       connect for 15 minutes (again to extend)
//   cloud off   disconnect and turn cellular off now
//   battery     reading the characteristic then returns charge and voltage
// The cloud stays up while an update is in progress, and up to 10 more minutes while one is pending.
//
// Cloud (while connected): event badge/led on every command, variable "hid",
// functions "type", "pair", "forget".

#include "Particle.h"
#include "ParticleHid.h"

using namespace phid;

SYSTEM_MODE(SEMI_AUTOMATIC);

SerialLogHandler logHandler(LOG_LEVEL_WARN, {{"hid", LOG_LEVEL_INFO}, {"app", LOG_LEVEL_INFO}});

BTKeyboard keyboard;
BTMediaKeys media;

namespace {

const BleUuid kLedService("f1465b1e-5f0b-4bf3-9226-f072b6f56489");
const BleUuid kLedCommand("83153295-8750-46cf-8630-d4bcd57865f6");

constexpr size_t kMaxCommand = 16;
char pending[kMaxCommand + 1];
volatile bool hasPending = false;
char current[kMaxCommand + 1] = "status";
bool publishCurrent = false;

void onLedWrite(const uint8_t* data, size_t len, const BlePeerDevice&, void*) {
    // BLE thread: copy and let loop() apply it.
    ATOMIC_BLOCK() {
        len = len > kMaxCommand ? kMaxCommand : len;
        memcpy(pending, data, len);
        pending[len] = 0;
        hasPending = true;
    }
}

BleCharacteristic ledChar("led", BleCharacteristicProperty::READ | BleCharacteristicProperty::WRITE, kLedCommand, kLedService,
        onLedWrite, nullptr);

bool parseColor(const char* s, uint32_t* rgb) {
    static const struct {
        const char* name;
        uint32_t rgb;
    } kNames[] = {{"on", 0xFFFFFF}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x00FF00}, {"blue", 0x0000FF},
            {"yellow", 0xFFC000}, {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}, {"orange", 0xFF4000}, {"purple", 0x8000FF},
            {"pink", 0xFF2080}, {"off", 0x000000}};
    for (const auto& n : kNames) {
        if (!strcasecmp(s, n.name)) {
            *rgb = n.rgb;
            return true;
        }
    }
    const char* hex = s[0] == '#' ? s + 1 : s;
    char* end = nullptr;
    const unsigned long v = strtoul(hex, &end, 16);
    if (strlen(hex) == 6 && end && *end == 0) {
        *rgb = (uint32_t)v;
        return true;
    }
    return false;
}

constexpr uint32_t kCloudAtBootMs = 5 * 60 * 1000;
constexpr uint32_t kCloudOnRequestMs = 15 * 60 * 1000;
constexpr uint32_t kCheckInEveryMs = 60 * 60 * 1000;  // counted from the end of the last cloud session
constexpr uint32_t kCheckInGiveUpMs = 3 * 60 * 1000;  // no signal: try again next hour
constexpr uint32_t kCheckInListenMs = 20 * 1000;      // the cloud says whether an update is pending right after connecting
constexpr uint32_t kPendingWaitMs = 10 * 60 * 1000;   // how long a pending update may keep the cloud up past its window
bool cloudWanted = false;
uint32_t cloudFrom = 0;
uint32_t cloudFor = 0;
uint32_t cloudEndedAt = 0;
bool checkingIn = false;
uint32_t checkInConnectedAt = 0;

void cloudOn(uint32_t ms) {
    cloudWanted = true;
    cloudFrom = millis();
    cloudFor = ms;
    checkingIn = false;
}

// Cellular costs about 4x everything else together, so it's only on when asked.
void manageCloud() {
    if (!cloudWanted) {
        if (millis() - cloudEndedAt < kCheckInEveryMs) {
            return;
        }
        Log.info("cloud check-in");
        cloudOn(kCheckInGiveUpMs);
        checkingIn = true;
        checkInConnectedAt = 0;
    }
    if (checkingIn && Particle.connected()) {
        if (!checkInConnectedAt) {
            checkInConnectedAt = millis();
        } else if (millis() - checkInConnectedAt >= kCheckInListenMs) {
            cloudFor = 0; // done, unless an update is pending (below)
        }
    }
    const int update = System.updateStatus();
    const bool updating = update == SYSTEM_UPDATE_STATUS_IN_PROGRESS ||
            (update == SYSTEM_UPDATE_STATUS_PENDING && millis() - cloudFrom < cloudFor + kPendingWaitMs);
    if (millis() - cloudFrom < cloudFor || updating) {
        if (!Particle.connected()) {
            Particle.connect(); // no-op while already connecting
        }
        return;
    }
    cloudWanted = false;
    checkingIn = false;
    cloudEndedAt = millis();
    Log.info("cloud off");
    Particle.disconnect();
    Cellular.off();
}

void showBattery() {
    FuelGauge fuel;
    char v[kMaxCommand + 1];
    snprintf(v, sizeof(v), "%.2f%% %.3fV", fuel.getNormalizedSoC(), fuel.getVCell());
    ledChar.setValue(v);
}

void applyLed(const char* cmd) {
    uint32_t rgb = 0;
    if (!strcasecmp(cmd, "cloud")) {
        cloudOn(kCloudOnRequestMs);
        return;
    } else if (!strcasecmp(cmd, "cloud off")) {
        cloudOn(0);
        return;
    } else if (!strcasecmp(cmd, "battery")) {
        showBattery();
        return;
    } else if (!strcasecmp(cmd, "status") || !strcasecmp(cmd, "auto")) {
        RGB.control(false); // back to the system status LED
    } else if (parseColor(cmd, &rgb)) {
        RGB.control(true);
        RGB.color(rgb);
    } else {
        Log.warn("led: unknown command \"%s\"", cmd);
        return;
    }
    strlcpy(current, cmd, sizeof(current));
    ledChar.setValue(current);
    publishCurrent = true;
}

int typeText(String text) {
    return (int)keyboard.print(text);
}

int pair(String ms) {
    const long d = ms.toInt();
    BleHid.startPairing(d > 0 ? (uint32_t)d : 120000);
    return 0;
}

int forget(String) {
    BleHid.forgetAll();
    return 0;
}

} // namespace

void setup() {
    Particle.function("type", typeText);
    Particle.function("pair", pair);
    Particle.function("forget", forget);
    Particle.variable("hid", [] {
        const HidStatus s = BleHid.status();
        const BleHidStats st = BleHid.stats();
        return String::format("{\"state\":%d,\"reason\":%d,\"bond\":%d,\"sec\":%d,\"bonds\":%d,\"conn\":%lu,\"reconnects\":%lu,"
                              "\"newBonds\":%lu,\"svcChanged\":%lu,\"scConfirmed\":%lu,\"scResult\":%lu,\"cccdWrites\":%lu,"
                              "\"sent\":%lu,\"led\":\"%s\"}",
                (int)s.state, (int)s.reason, s.bond, s.security, s.bondCount, st.connections, st.bondedReconnects, st.newBonds,
                st.serviceChanged, st.scConfirmed, st.scLastResult, st.cccdWrites, st.reports.sent, current);
    });

    BleHid.name("HAVEN-BADGE-1").add(keyboard).add(media);
    const int r = BleHid.begin();
    Log.info("BleHid.begin: %d", r);

    // Add app services after BleHid.begin() so the HID handles stay put when you
    // add or change them; hosts that cached the old table keep typing meanwhile.
    BLE.addCharacteristic(ledChar);
    ledChar.setValue(current);

    cloudOn(kCloudAtBootMs);
}

void loop() {
    if (hasPending) {
        char cmd[kMaxCommand + 1];
        ATOMIC_BLOCK() {
            memcpy(cmd, pending, sizeof(cmd));
            hasPending = false;
        }
        applyLed(cmd);
    }
    manageCloud();
    static uint32_t lastPublish;
    if (publishCurrent && Particle.connected() && millis() - lastPublish >= 1100) {
        if (Particle.publish("badge/led", current)) {
            publishCurrent = false;
            lastPublish = millis();
        }
    }
}
