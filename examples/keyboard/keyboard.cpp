// Keyboard + media keys. Wire buttons from D2, D3, D4 to GND.
//   D2: type a line   D3: play/pause   D4 held at boot or for 3 s: pair a new host
// The first host pairs automatically; afterwards it reconnects on its own.

#include "Particle.h"
#include "ParticleHid.h"

using namespace phid;

SerialLogHandler logHandler(LOG_LEVEL_WARN, {{"hid", LOG_LEVEL_INFO}});

BTKeyboard keyboard;
BTMediaKeys media;

void onStatus(const HidStatus& s) {
    Log.info("hid: state %d reason %d bond %d pairing %d", (int)s.state, (int)s.reason, s.bond, s.pairing);
}

void setup() {
    pinMode(D2, INPUT_PULLUP);
    pinMode(D3, INPUT_PULLUP);
    pinMode(D4, INPUT_PULLUP);

    BleHid.name("Boron Keys").add(keyboard).add(media);
    BleHid.onStatus(onStatus);
    if (BleHid.begin() != 0) {
        Log.error("BleHid.begin failed: reason %d", (int)BleHid.status().reason);
    }
    if (digitalRead(D4) == LOW) {
        BleHid.startPairing();
    }
    keyboard.onLeds([](uint8_t leds) { digitalWrite(D7, (leds & Led::CapsLock) ? HIGH : LOW); });
    pinMode(D7, OUTPUT);
}

void loop() {
    static bool d2, d3;
    static uint32_t d4Since;

    const bool now2 = digitalRead(D2) == LOW;
    if (now2 && !d2) {
        keyboard.println("Hello from a Boron!");
    }
    d2 = now2;

    const bool now3 = digitalRead(D3) == LOW;
    if (now3 && !d3) {
        media.tap(Media::PlayPause);
    }
    d3 = now3;

    if (digitalRead(D4) == LOW) {
        if (d4Since && millis() - d4Since > 3000 && !BleHid.pairing()) {
            BleHid.startPairing();
        }
        if (!d4Since) {
            d4Since = millis() | 1;
        }
    } else {
        d4Since = 0;
    }
    delay(10);
}
