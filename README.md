# particle-hid

Native Bluetooth LE keyboards, mice, media keys, and gamepads on nRF52840 Particle devices. Built and tested on the Boron BRN404X. The computer sees an ordinary Bluetooth keyboard or controller: pair once, and it reconnects by itself after either side restarts. Nothing runs on the host.

```cpp
#include "Particle.h"
#include "ParticleHid.h"

BTKeyboard keyboard;
BTMediaKeys media;

void setup() {
    BleHid.name("Bagel Keys").add(keyboard).add(media);
    BleHid.begin(); // never waits for a host
}

void loop() {
    if (buttonPressed()) keyboard.println("hello");
    if (knobTurned()) media.tap(phid::Media::VolumeUp);
}
```

It has two parts:

1. **This library**, which you add to your firmware. It owns the HID profile, the report queues, pairing, and the bond store.
2. **A small Device OS patch** (+384 bytes), flashed once per device. Stock Device OS can't keep Bluetooth bonds, and HID needs them. See [`device-os/README.md`](device-os/README.md) for what the patch changes and how it was checked.

## Tested

On a Boron BRN404X running the patched 6.6.0 system part, with macOS:

- Paired once, then typed text from a cloud function call. All 106 reports for 48 characters arrived, with no disconnects.
- Reconnected without a pairing prompt after the Mac's Bluetooth was toggled (140 ms from connect to ready), and after the Boron rebooted twice (152 and 192 ms).
- Caps Lock LED reports reached the device from the host.
- Adding a custom BLE service changed the attribute table. The Mac confirmed Service Changed, rediscovered, and kept typing on the same bond.

Not yet tested: Windows, iOS, Android, Linux, the mouse and gamepad on hardware, listening mode, and long soaks.

## Use it in your firmware

### 1. Put the patched system part on the device (once)

Download `boron-system-part1@6.6.0+phid.1.bin` and its `.sha256` from the [latest release](https://github.com/lordbagel42/particle-hid/releases/latest). The device needs Device OS 6.6.0 first:

```sh
sha256sum -c boron-system-part1@6.6.0+phid.1.bin.sha256
particle update                                          # skip if the device already runs 6.6.0
particle flash --usb boron-system-part1@6.6.0+phid.1.bin
```

To flash over the cloud instead, or to go back to stock, see [`device-os/README.md`](device-os/README.md#install-it-on-a-device).

### 2. Add the library to your project

In a Particle project (the layout `particle project create` and Workbench use), put the library under `lib/`. A git submodule keeps it updatable:

```sh
cd my-project
git submodule add https://github.com/lordbagel42/particle-hid.git lib/particle-hid
```

```text
my-project/
  project.properties
  src/my-project.cpp
  lib/particle-hid/      <- library.properties, src/, ...
```

Workbench, `particle compile`, and local builds pick up `lib/*/src` automatically. Don't also list `particle-hid` under `dependencies` in `project.properties`: it isn't in the Particle library registry.

### 3. Use it

```cpp
#include "Particle.h"
#include "ParticleHid.h"   // BleHid, BTKeyboard, BTMouse, BTMediaKeys, BTSystemControl, BTGamepad

BTKeyboard keyboard;

void setup() {
    BleHid.name("My Keyboard").add(keyboard);
    if (BleHid.begin() != 0) {
        Log.error("BLE HID unavailable: reason %d", (int)BleHid.status().reason);
    }
}

void loop() {
    if (BleHid.ready() && somethingHappened()) {
        keyboard.tap(phid::Key::Enter);
    }
}
```

Key names, media keys, and buttons live in the `phid` namespace (`phid::Key::A`, `phid::Media::PlayPause`, `phid::Button::A`). Add `using namespace phid;` if you prefer.

### 4. Build and flash it for 6.6.0

```sh
particle compile boron . --target 6.6.0 --saveTo firmware.bin
particle flash --usb firmware.bin                         # or: particle flash <device> firmware.bin
```

Always build for **6.6.0**. The patch is a 6.6.0 system part, and firmware built for a newer Device OS makes the cloud replace it with stock.

### 5. Pair

With no bonds, the device opens pairing on its own and advertises under your name. Pair it from the computer's Bluetooth settings; there is no PIN. To pair another host later, call `BleHid.startPairing()`, for example from a button held at boot (see [`examples/keyboard`](examples/keyboard/keyboard.cpp)).

## How it fits with the rest of your firmware

- **Nothing blocks.** `begin()` returns at once, and input calls (`tap`, `print`, `move`, ...) only queue. A worker thread sends the reports. Call them from `loop()` or any application thread, not from an ISR.
- **It coexists with cellular and the cloud.** On the Boron, `Particle.publish`, functions, and OTA keep working while HID is connected; the bring-up example uses all of them.
- **USB HID stays available.** The classes are prefixed `BT` so Device OS's own USB `Keyboard` and `Mouse` still work in the same app.
- **Your own BLE services work too.** Use Device OS's normal `BLE` API next to `BleHid`, over the same bonded connection. Add your characteristics **after** `BleHid.begin()`, so adding or changing them later doesn't move the HID attributes. When the attribute table changes, bonded hosts get Service Changed and rediscover it. [`examples/haven-badge`](examples/haven-badge/haven-badge.cpp) adds an LED characteristic, and [`tools/badge-led`](tools/badge-led/badge-led.swift) is a macOS CLI that sets it over the existing connection.
- **Listening mode** takes over advertising while it's active. The library restores its own advertising afterwards (not yet tested on hardware).
- **Storage:** bonds are kept in `/usr/phid` on the device's filesystem, one small file per host (204 bytes).
- **Resources:** the whole keyboard example app is about 29 KB of flash and 4 KB of static RAM, plus a 3 KB worker thread stack.

### On stock Device OS

The library never half-works. On a system part without the patch, `begin()` returns `SYSTEM_ERROR_NOT_SUPPORTED`, and `status().reason` is `HidReason::NeedsPatchedDeviceOS`. The rest of your app keeps running, so firmware with `particle-hid` can safely reach devices that aren't patched yet.

For bring-up only, `BleHid.allowUnbonded(true)` runs without the patch and without bonding. Linux usually accepts that; other hosts usually don't, and LED and Protocol Mode writes aren't seen.

## API

`BleHid` (a single instance, like `BLE`). Configure before `begin()`:

| Call | |
| --- | --- |
| `name(str)` (up to 20 bytes), `manufacturer`, `model`, `appearance` | Identity. The appearance comes from the first device unless you set it. |
| `pnpId(source, vid, pid, version)` | PnP ID. Set your own VID/PID before shipping: hosts cache mappings on it. |
| `add(device)` | Report IDs follow the order you add devices. |
| `connection(ConnectionPreset::LowLatency / Balanced / LowPower)` | Preferred connection interval. |
| `maxBonds(n)` (1–8, default 4), `evictOldestBond(bool)`, `autoPairWhenEmpty(bool)` | Bond policy. |
| `battery(BatterySource::Auto / Manual / None)` | Battery service. Auto reads the fuel gauge. |
| `readyDeadline(ms)` | How long input waits for a host before it's dropped (default 2000). |

At runtime:

| Call | |
| --- | --- |
| `begin()`, `end()` | `begin()` returns 0 or a `SYSTEM_ERROR_*` code. |
| `startPairing(ms = 120000)`, `stopPairing()`, `pairing()` | The pairing window. It closes when a host bonds. |
| `bondCount()`, `forget(slot)`, `forgetAll()` | Bonds. |
| `batteryLevel(percent)` | With `BatterySource::Manual`. |
| `ready()`, `waitReady(ms)` | True when reports reach the host. |
| `status()`, `onStatus(cb)` | State (`Off`, `Advertising`, `Connected`, `Ready`, `Error`), reason, bond, security level. |
| `stats()` | Counters: connections, reconnects, new bonds, security failures, Service Changed, reports sent and dropped. |
| `deviceOsSupported()` | True when the running system part has the patch. |

Devices:

- **`BTKeyboard`** (a `Print`): `press`, `release`, `releaseAll`, `tap(key, mods)`, `type(str)`, `print`/`println`/`printf`, `leds()`/`onLeds(cb)`. 6-key rollover. US layout (the only one built in so far; `layout(...)` takes your own). Also works in boot protocol (BIOS, some TVs).
- **`BTMouse`**: `move(dx, dy, wheel, pan)`, `scroll`, `press`/`release`/`click`. 5 buttons, 16-bit motion.
- **`BTMediaKeys`** (consumer controls) and **`BTSystemControl`** (power, sleep, wake): `press`, `release`, `tap`.
- **`BTGamepad(GamepadLayout().buttons(16).hat().sticks(2).triggers())`**: `press`/`release`/`button`, `hat`, `stick` (int16 or float −1..1), `trigger` (uint8 or float 0..1).

Callbacks (`onStatus`, `onLeds`) run on the HID worker thread. Keep them short, and don't call `Particle.publish` from them; set a flag for `loop()` instead.

## Behavior you can rely on

- **Keys never stick.** Every press and release reaches the host in order, one report each. A full queue refuses new presses, never releases.
- **Nothing is dropped silently.** Input waits up to `readyDeadline` for a host, then is dropped and counted in `stats()`. Held keys and buttons stay held across a disconnect and are restated when the host is back.
- **Mouse motion is never lost or duplicated.** Moves are summed and sent in clamped chunks, and a click lands after the motion before it.
- **A gamepad tap shorter than a connection interval still registers.**
- **Pairing is gated.** Hosts can pair only inside the pairing window, which opens automatically while there are no bonds.
- **Bond writes are atomic** (tmp file + rename), and a corrupt record is discarded, never fatal.
- **Profile changes are safe.** Each bond stores a hash of the attribute table. When a firmware update changes it, returning hosts get Service Changed, and the bond only takes the new hash once the host confirms.

## Examples

| | |
| --- | --- |
| [`keyboard`](examples/keyboard/keyboard.cpp) | Buttons that type a line and play/pause; Caps Lock LED on D7 |
| [`mouse`](examples/mouse/mouse.cpp) | Draws circles; click on a button |
| [`gamepad`](examples/gamepad/gamepad.cpp) | Buttons, a thumbstick, and a trigger |
| [`cloud-bringup`](examples/cloud-bringup/cloud-bringup.cpp) | Keyboard driven and observed over the Particle cloud, for remote testing |
| [`haven-badge`](examples/haven-badge/haven-badge.cpp) | Keyboard plus a custom LED service, used with `tools/badge-led` |

Build one with `particle compile boron examples/keyboard --target 6.6.0`.

## Repository

```text
src/                  the library
examples/             example apps
device-os/            the Device OS patch, build script, and binary comparison
scripts/              flash-usb.sh and flash-cloud.sh (Boron)
tools/badge-led/      macOS CLI for the haven-badge example
test/native/          host tests for the portable core: make -C test/native
```

## License

The library is MIT. The Device OS patch is LGPL-3.0, like Device OS.
