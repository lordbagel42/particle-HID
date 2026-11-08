// Remote bring-up: keyboard + media keys, observable and drivable from the Particle cloud.
//
// Events (watch with scripts/flash-cloud.sh watch <device>):
//   phid/boot    Device OS version, PHID ABI present, begin() result
//   phid/status  every HID state change (state, reason, bond, security level, pairing, bonds)
//   phid/leds    host keyboard LEDs (toggle Caps Lock on the host: proves host -> device reports)
// Functions:
//   type   text to type on the host     pair   open pairing (ms, default 120000)
//   media  "play", "next", "prev", "mute", "up", "down"    forget  forget all bonds
//   key    "capslock", "numlock", "enter", "escape", "backspace", "tab"; "capslock:300" holds 300 ms
// Variable:
//   hid    JSON status and counters

#include "Particle.h"
#include "ParticleHid.h"

using namespace phid;

SerialLogHandler logHandler(LOG_LEVEL_WARN, {{"hid", LOG_LEVEL_INFO}, {"app", LOG_LEVEL_INFO}});

BTKeyboard keyboard;
BTMediaKeys media;

namespace {

int beginResult = 0;
bool bootPublished = false;
volatile bool statusChanged = true;
volatile int leds = -1;
int ledsPublished = -1;
uint32_t lastPublish = 0;
char hidJson[512];

const char* stateName(HidState s) {
    switch (s) {
    case HidState::Off: return "off";
    case HidState::Advertising: return "advertising";
    case HidState::Connected: return "connected";
    case HidState::Ready: return "ready";
    case HidState::Error: return "error";
    }
    return "?";
}

const char* reasonName(HidReason r) {
    switch (r) {
    case HidReason::None: return "none";
    case HidReason::NeedsPatchedDeviceOS: return "needs-patched-device-os";
    case HidReason::UnsupportedPlatform: return "unsupported-platform";
    case HidReason::ProfileInvalid: return "profile-invalid";
    case HidReason::GattFailed: return "gatt-failed";
    case HidReason::BleStackFailed: return "ble-stack-failed";
    case HidReason::AlreadyStarted: return "already-started";
    }
    return "?";
}

void formatStatus(char* out, size_t n) {
    const HidStatus s = BleHid.status();
    const BleHidStats st = BleHid.stats();
    JSONBufferWriter w(out, n - 1);
    w.beginObject();
    w.name("state").value(stateName(s.state));
    w.name("reason").value(reasonName(s.reason));
    w.name("bond").value((int)s.bond);
    w.name("sec").value((int)s.security);
    w.name("pairing").value(s.pairing);
    w.name("boot").value(s.bootProtocol);
    w.name("bonds").value((int)s.bondCount);
    w.name("conn").value((unsigned)st.connections);
    w.name("disc").value((unsigned)st.disconnects);
    w.name("lastDisc").value((int)st.lastDisconnectReason);
    w.name("reconnects").value((unsigned)st.bondedReconnects);
    w.name("newBonds").value((unsigned)st.newBonds);
    w.name("secFail").value((unsigned)st.securityFailures);
    w.name("lastSec").value((int)st.lastSecurityStatus);
    w.name("svcChanged").value((unsigned)st.serviceChanged);
    w.name("storeErr").value((unsigned)st.storeErrors);
    w.name("readyMs").value((unsigned)st.lastTimeToReadyMs);
    w.name("sent").value((unsigned)st.reports.sent);
    w.name("ledWrites").value((unsigned)st.outputReports);
    w.name("leds").value((int)keyboard.leds());
    w.endObject();
    w.buffer()[std::min(w.bufferSize(), w.dataSize())] = 0;
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

int tapKey(String k) {
    const struct {
        const char* name;
        Key key;
    } keys[] = {{"capslock", Key::CapsLock}, {"numlock", Key::NumLock}, {"enter", Key::Enter},
            {"escape", Key::Escape}, {"backspace", Key::Backspace}, {"tab", Key::Tab}};
    // "name" taps; "name:ms" holds the key down for ms (macOS ignores very short Caps Lock presses).
    const int colon = k.indexOf(':');
    const String name = colon < 0 ? k : k.substring(0, colon);
    const long holdMs = colon < 0 ? 0 : k.substring(colon + 1).toInt();
    for (const auto& e : keys) {
        if (name.equalsIgnoreCase(e.name)) {
            if (holdMs <= 0) {
                return keyboard.tap(e.key) ? 0 : -1;
            }
            if (!keyboard.press(e.key)) {
                return -1;
            }
            delay(holdMs > 2000 ? 2000 : holdMs);
            return keyboard.release(e.key) ? 0 : -1;
        }
    }
    return -2;
}

int mediaKey(String k) {
    const struct {
        const char* name;
        Media m;
    } keys[] = {{"play", Media::PlayPause}, {"next", Media::NextTrack}, {"prev", Media::PreviousTrack},
            {"mute", Media::Mute}, {"up", Media::VolumeUp}, {"down", Media::VolumeDown}};
    for (const auto& e : keys) {
        if (k.equalsIgnoreCase(e.name)) {
            return media.tap(e.m) ? 0 : -1;
        }
    }
    return -2;
}

} // namespace

void setup() {
    Particle.function("type", typeText);
    Particle.function("pair", pair);
    Particle.function("forget", forget);
    Particle.function("media", mediaKey);
    Particle.function("key", tapKey);
    Particle.variable("hid", [] {
        formatStatus(hidJson, sizeof(hidJson));
        return String(hidJson);
    });

    BleHid.name("Boron HID").add(keyboard).add(media);
    BleHid.onStatus([](const HidStatus&) { statusChanged = true; }); // worker thread: just flag it
    keyboard.onLeds([](uint8_t l) { leds = l; });
    beginResult = BleHid.begin();
    Log.info("begin: %d, ABI: %d", beginResult, (int)BleHid.deviceOsSupported());
}

void loop() {
    if (!Particle.connected() || millis() - lastPublish < 1100) {
        return; // stay under the cloud's 1 event/s limit
    }
    char buf[600];
    if (!bootPublished) {
        snprintf(buf, sizeof(buf), "{\"deviceOs\":\"%s\",\"phidAbi\":%s,\"begin\":%d,\"reason\":\"%s\"}",
                System.version().c_str(), BleHid.deviceOsSupported() ? "true" : "false", beginResult,
                reasonName(BleHid.status().reason));
        bootPublished = Particle.publish("phid/boot", buf);
    } else if (leds != ledsPublished) {
        const int l = leds;
        snprintf(buf, sizeof(buf), "{\"leds\":%d,\"caps\":%s,\"num\":%s}", l, (l & Led::CapsLock) ? "true" : "false",
                (l & Led::NumLock) ? "true" : "false");
        if (Particle.publish("phid/leds", buf)) {
            ledsPublished = l;
        }
    } else if (statusChanged) {
        statusChanged = false;
        formatStatus(buf, sizeof(buf));
        if (!Particle.publish("phid/status", buf)) {
            statusChanged = true;
        }
    } else {
        return;
    }
    lastPublish = millis();
}
