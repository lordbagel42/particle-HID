// Gamepad: buttons on D2-D5 (to GND), a thumbstick on A0/A1, a trigger pot on A2.

#include "Particle.h"
#include "ParticleHid.h"

using namespace phid;

SerialLogHandler logHandler(LOG_LEVEL_WARN, {{"hid", LOG_LEVEL_INFO}});

BTGamepad pad(GamepadLayout().buttons(8).hat(false).sticks(1).triggers(true));

const uint16_t pins[] = {D2, D3, D4, D5};
const uint8_t buttons[] = {Button::A, Button::B, Button::X, Button::Y};

float axis(uint16_t pin) {
    return (analogRead(pin) - 2048) / 2048.0f; // 12-bit ADC centered at mid-scale
}

void setup() {
    for (uint16_t p : pins) {
        pinMode(p, INPUT_PULLUP);
    }
    BleHid.name("Boron Pad").add(pad).connection(ConnectionPreset::LowLatency);
    BleHid.begin();
}

void loop() {
    // Set the whole state every pass; the library sends only changes, and
    // a tap shorter than one connection interval still reaches the host.
    for (size_t i = 0; i < 4; ++i) {
        pad.button(buttons[i], digitalRead(pins[i]) == LOW);
    }
    pad.stick(Stick::Left, axis(A0), -axis(A1));
    pad.trigger(Trigger::Right, analogRead(A2) / 4095.0f);
    delay(5);
}
