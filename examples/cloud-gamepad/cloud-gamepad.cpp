// Remote gamepad test: a Bluetooth gamepad driven from the Particle cloud, for
// checking it in a browser gamepad tester without wiring any controls.
//
// Functions:
//   pad     commands separated by ";", run in order:
//             press <n> | release <n> | tap <n> [ms]   buttons 1-16
//             hat <up|upright|right|downright|down|downleft|left|upleft|center>
//             stick <l|r> <x> <y>                      -1..1, +y is down
//             trigger <l|r> <v>                        0..1
//             wait <ms> | reset
//           e.g. "press 1; stick l 1 0; wait 500; reset"
//   demo    "on" loops a sweep of every control (~11 s), "off" stops and centers
//   pair    open pairing (ms, default 120000)       forget  forget all bonds
// Variable:
//   hid     JSON status: state, bonds, reports sent, demo on/off

#include "Particle.h"
#include "ParticleHid.h"

using namespace phid;

SerialLogHandler logHandler(LOG_LEVEL_WARN, {{"hid", LOG_LEVEL_INFO}, {"app", LOG_LEVEL_INFO}});

// The gamepad must be the first (or only) device: browsers on macOS only list
// HID devices whose primary usage is a gamepad or joystick.
BTGamepad pad(GamepadLayout().buttons(16).hat().sticks(2).triggers());

namespace {

constexpr size_t kMaxScript = 600;
char script[kMaxScript + 1];
volatile bool scriptPending = false;
volatile bool demoOn = false;

const struct {
    const char* name;
    Hat hat;
} kHats[] = {{"up", Hat::Up}, {"upright", Hat::UpRight}, {"right", Hat::Right}, {"downright", Hat::DownRight},
        {"down", Hat::Down}, {"downleft", Hat::DownLeft}, {"left", Hat::Left}, {"upleft", Hat::UpLeft},
        {"center", Hat::Centered}};

void center() {
    pad.releaseAll();
    pad.hat(Hat::Centered);
    pad.stick(Stick::Left, 0.0f, 0.0f);
    pad.stick(Stick::Right, 0.0f, 0.0f);
    pad.trigger(Trigger::Left, 0.0f);
    pad.trigger(Trigger::Right, 0.0f);
}

// Milliseconds clamped to 0-5000, so a typo can't stall loop() for days.
uint32_t ms(const char* s, uint32_t dflt) {
    const long v = s ? atol(s) : (long)dflt;
    return v < 0 ? 0 : v > 5000 ? 5000 : (uint32_t)v;
}

// Button number 1-16, or 0 when out of range.
uint8_t buttonNumber(const char* s) {
    const long v = s ? atol(s) : 0;
    return v >= 1 && v <= 16 ? (uint8_t)v : 0;
}

// Runs one command; returns false when it's not understood.
bool run(char* cmd) {
    char* save = nullptr;
    const char* op = strtok_r(cmd, " \t", &save);
    const char* a = strtok_r(nullptr, " \t", &save);
    const char* b = strtok_r(nullptr, " \t", &save);
    const char* c = strtok_r(nullptr, " \t", &save);
    if (!op) {
        return true;
    }
    const bool left = a && (a[0] == 'l' || a[0] == 'L');
    const uint8_t button = buttonNumber(a);
    if (!strcasecmp(op, "press") && button) {
        return pad.press(button);
    } else if (!strcasecmp(op, "release") && button) {
        return pad.release(button);
    } else if (!strcasecmp(op, "tap") && button) {
        pad.press(button);
        delay(ms(b, 100));
        return pad.release(button);
    } else if (!strcasecmp(op, "hat") && a) {
        for (const auto& h : kHats) {
            if (!strcasecmp(a, h.name)) {
                pad.hat(h.hat);
                return true;
            }
        }
    } else if (!strcasecmp(op, "stick") && a && b && c) {
        pad.stick(left ? Stick::Left : Stick::Right, (float)atof(b), (float)atof(c));
        return true;
    } else if (!strcasecmp(op, "trigger") && a && b) {
        pad.trigger(left ? Trigger::Left : Trigger::Right, (float)atof(b));
        return true;
    } else if (!strcasecmp(op, "wait") && a) {
        delay(ms(a, 0));
        return true;
    } else if (!strcasecmp(op, "reset")) {
        center();
        return true;
    }
    Log.warn("pad: can't run \"%s\"", op);
    return false;
}

// One pass of the demo: each button, the hat around, both sticks in a circle,
// then both triggers up and down. Returns early when the demo is turned off.
void demoPass() {
    for (uint8_t i = 1; i <= 16 && demoOn; ++i) {
        pad.press(i);
        delay(250);
        pad.release(i);
        delay(50);
    }
    for (const auto& h : kHats) {
        if (!demoOn) {
            return;
        }
        pad.hat(h.hat);
        delay(250);
    }
    for (int step = 0; step <= 72 && demoOn; ++step) {
        const float t = step * (2.0f * (float)M_PI / 72.0f);
        pad.stick(Stick::Left, cosf(t), sinf(t));
        pad.stick(Stick::Right, -cosf(t), sinf(t));
        delay(30);
    }
    pad.stick(Stick::Left, 0.0f, 0.0f);
    pad.stick(Stick::Right, 0.0f, 0.0f);
    for (int step = 0; step <= 40 && demoOn; ++step) {
        const float v = step <= 20 ? step / 20.0f : (40 - step) / 20.0f;
        pad.trigger(Trigger::Left, v);
        pad.trigger(Trigger::Right, 1.0f - v);
        delay(30);
    }
    pad.trigger(Trigger::Left, 0.0f);
    pad.trigger(Trigger::Right, 0.0f);
}

int padCommand(String s) {
    if (scriptPending || s.length() > kMaxScript) {
        return -1;
    }
    strlcpy(script, s.c_str(), sizeof(script));
    scriptPending = true;
    return 0;
}

int demo(String s) {
    demoOn = !s.equalsIgnoreCase("off");
    return demoOn;
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
    Particle.function("pad", padCommand);
    Particle.function("demo", demo);
    Particle.function("pair", pair);
    Particle.function("forget", forget);
    Particle.variable("hid", [] {
        const HidStatus s = BleHid.status();
        const BleHidStats st = BleHid.stats();
        return String::format("{\"state\":%d,\"reason\":%d,\"bond\":%d,\"bonds\":%d,\"pairing\":%d,\"conn\":%lu,"
                              "\"sent\":%lu,\"busy\":%lu,\"demo\":%d}",
                (int)s.state, (int)s.reason, s.bond, s.bondCount, (int)s.pairing, st.connections, st.reports.sent,
                st.reports.busy, (int)demoOn);
    });

    BleHid.name("HAVEN-PAD").add(pad).connection(ConnectionPreset::LowLatency);
    Log.info("BleHid.begin: %d", BleHid.begin());
}

void loop() {
    if (scriptPending) {
        char* save = nullptr;
        for (char* cmd = strtok_r(script, ";", &save); cmd; cmd = strtok_r(nullptr, ";", &save)) {
            run(cmd);
        }
        scriptPending = false;
    }
    if (demoOn) {
        demoPass();
        if (!demoOn) {
            center();
        }
    }
}
