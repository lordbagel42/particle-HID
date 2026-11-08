// Mouse that draws slow circles while D2 is held to GND, and clicks on D3.

#include <cmath>

#include "Particle.h"
#include "ParticleHid.h"

using namespace phid;

SerialLogHandler logHandler(LOG_LEVEL_WARN, {{"hid", LOG_LEVEL_INFO}});

BTMouse mouse;

void setup() {
    pinMode(D2, INPUT_PULLUP);
    pinMode(D3, INPUT_PULLUP);
    BleHid.name("Boron Mouse").add(mouse).connection(ConnectionPreset::LowLatency);
    BleHid.begin();
}

void loop() {
    static float angle;
    static bool d3;

    if (digitalRead(D2) == LOW && BleHid.ready()) {
        angle += 0.05f;
        mouse.move((int32_t)lroundf(6 * cosf(angle)), (int32_t)lroundf(6 * sinf(angle)));
    }
    const bool now3 = digitalRead(D3) == LOW;
    if (now3 != d3) {
        now3 ? mouse.press(MouseButton::Left) : mouse.release(MouseButton::Left);
    }
    d3 = now3;
    delay(15);
}
