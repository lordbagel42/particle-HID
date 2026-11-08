// particle-hid: native Bluetooth LE HID (keyboard, mouse, media keys, gamepad)
// for nRF52840 Particle devices. Needs the patched system-part1 in device-os/.
#pragma once

#include "phid/ble_hid.h"
#include "phid/gamepad.h"
#include "phid/hid_codes.h"
#include "phid/keyboard.h"
#include "phid/keyboard_layout.h"
#include "phid/mouse.h"
#include "phid/usage_keys.h"

using phid::BleHid;
using phid::BTGamepad;
using phid::BTKeyboard;
using phid::BTMediaKeys;
using phid::BTMouse;
using phid::BTSystemControl;
