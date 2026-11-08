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
2. **A small Device OS patch** (+384 bytes). Stock Device OS can't keep Bluetooth bonds, and HID needs them. The library ships the patched system part and installs it on the device's first boot, so you don't flash it separately. See [`device-os/README.md`](device-os/README.md) for what the patch changes and how it was checked.

## Tested

On a Boron BRN404X running the patched 6.6.0 system part, with macOS:

- Paired once, then typed text from a cloud function call. All 106 reports for 48 characters arrived, with no disconnects.
- Reconnected without a pairing prompt after the Mac's Bluetooth was toggled (140 ms from connect to ready), and after the Boron rebooted twice (152 and 192 ms).
- Caps Lock LED reports reached the device from the host.
- Self-install: with stock 6.6.0 on the Boron, the bundled patch installed and applied on the first boot (twice, from two separate stock restores), and bonds were back afterwards. A deliberately corrupted asset failed validation without a reset, was tried twice across reboots, and then left alone.
- Adding a custom BLE service changed the attribute table. The Mac confirmed Service Changed, rediscovered, and kept typing on the same bond.

Not yet tested: Windows, iOS, Android, Linux, the mouse and gamepad on hardware, listening mode, and long soaks.

## Use it in your firmware

### 1. Add the library to your project

In a Particle project (the layout `particle project create` and Workbench use), put the library under `lib/`. A git submodule keeps it updatable:

```sh
cd my-project
git submodule add https://github.com/lordbagel42/particle-hid.git lib/particle-hid
```

```text
my-project/
  project.properties
  src/my-project.cpp
  lib/particle-hid/      <- library.properties, src/, assets/, ...
```

Workbench, `particle compile`, and local builds pick up `lib/*/src` automatically. Don't also list `particle-hid` under `dependencies` in `project.properties`: it isn't in the Particle library registry.

### 2. Bundle the Device OS patch

Add one line to `project.properties`:

```properties
assetOtaDir=lib/particle-hid/assets
```

That ships the patched system part with your firmware as an [Asset OTA](https://docs.particle.io/reference/asset-ota/asset-ota/) asset. When `BleHid.begin()` finds stock Device OS 6.6.0 and the bundled patch, it installs the patch and the device resets once. Nothing else to flash, whether you flash one device or release to a product fleet.

The bundled patch is for the **Boron** only. On other nRF52840 platforms, leave `assetOtaDir` out (the asset would only take up asset storage) and flash a patch built for that platform by hand. Devices with Device Protection can't install it from the app.

If your project already has an `assetOtaDir`, copy `lib/particle-hid/assets/boron-system-part1@6.6.0+phid.1.bin` into it instead. The library looks for an asset whose name contains `+phid` and ends in `.bin`.

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

With assets, `particle compile` writes a `.zip` (your app plus the patch). Flash or upload that:

```sh
particle compile boron . --target 6.6.0 --saveTo firmware.zip
particle flash <device> firmware.zip      # or upload firmware.zip as product firmware in the console
```

Always build for **6.6.0**. The patch is a 6.6.0 system part, and firmware built for a newer Device OS makes the cloud replace it with stock.

On a device that isn't patched yet, the first boot goes like this:

1. The cloud flashes the app, then the device downloads the asset in safe mode (breathing magenta).
2. The app starts. `BleHid.begin()` copies the patch into the OTA area (about 13 s on a Boron, LED blinking magenta), checks its integrity and dependencies, and returns `SYSTEM_ERROR_NOT_SUPPORTED` with `status().reason == HidReason::InstallingPatch`.
3. Device OS resets and applies it. From then on `begin()` starts HID normally, and later app updates with the same asset don't download or install it again.

The install only runs when the asset replaces the running system part of the same platform and version (6.6.0), so it never downgrades or crosses Device OS versions. A patch that fails its checks is discarded without a reset. Each asset gets at most two attempts, and at most one per boot, so a bad one can't put the device in a loop; pressing RESET or power-cycling allows one more. `BleHid.installPatch(false)` turns it off.

It only installs onto stock Device OS. A device that already runs a patched system part keeps it, even if a later library release bundles a newer one.

To flash the patch by hand instead (USB, or without assets), see [`device-os/README.md`](device-os/README.md#install-it-on-a-device).

### 5. Pair

With no bonds, the device opens pairing on its own and advertises under your name. Pair it from the computer's Bluetooth settings; there is no PIN.

To pair another host later, or to pair again after removing the device on the computer, call `BleHid.startPairing()`, for example from a button held at boot (see [`examples/keyboard`](examples/keyboard/keyboard.cpp)). The device holds one connection at a time, so a host that's still connected (or reconnects) keeps it; turn Bluetooth off on that host while you pair a new one.

Outside pairing mode the device is invisible to new hosts: it advertises only to bonded hosts (a SoftDevice whitelist, which also resolves their private addresses), isn't discoverable, and ignores connection and scan requests from anyone else. With no bonds and pairing closed, it doesn't advertise at all.

## How it fits with the rest of your firmware

- **Nothing blocks.** `begin()` returns at once (except for the one-time patch install above, about 13 s), and input calls (`tap`, `print`, `move`, ...) only queue. A worker thread sends the reports. Call them from `loop()` or any application thread, not from an ISR.
- **It coexists with cellular and the cloud.** On the Boron, `Particle.publish`, functions, and OTA keep working while HID is connected; the bring-up example uses all of them.
- **USB HID stays available.** The classes are prefixed `BT` so Device OS's own USB `Keyboard` and `Mouse` still work in the same app.
- **Your own BLE services work too.** Use Device OS's normal `BLE` API next to `BleHid`, over the same bonded connection. Add your characteristics **after** `BleHid.begin()`, so adding or changing them later doesn't move the HID attributes. When the attribute table changes, bonded hosts get Service Changed and rediscover it. [`examples/haven-badge`](examples/haven-badge/haven-badge.cpp) adds an LED characteristic, and [`tools/badge-led`](tools/badge-led/badge-led.swift) is a macOS CLI that sets it over the existing connection.
- **Status LED.** The RGB LED shows Bluetooth events as short overlays, then goes back to the normal Device OS status (breathing cyan and so on). Listening mode, updates, `RGB.control(true)`, and Device OS's own important statuses take precedence; your own `LEDStatus` at `LED_PRIORITY_IMPORTANT` gets interrupted during an event.

  | Pattern | Meaning |
  | --- | --- |
  | Blue double blink every 2 s | Pairing mode: advertising for a new host |
  | 3 quick blue flashes | A known host connected and reports are flowing |
  | Green for 1 s | A new host paired and is ready |
  | 2 orange flashes | The host disconnected |
  | 3 fast red flashes | Pairing was refused or failed (for example, outside the pairing window) |

  `BleHid.statusLed(D7)` uses a plain LED instead: the same timing without colour, plus solid on while a host is connected and ready (so pick a pin your app doesn't use; the keyboard example uses D7 for Caps Lock). `BleHid.statusLed(StatusLed::None)` turns it off. Set these before `begin()`.
- **Listening mode** takes over advertising while it's active. The library restores its own advertising afterwards (not yet tested on hardware).
- **Storage:** bonds are kept in `/usr/phid` on the device's filesystem, one small file per host (204 bytes).
- **Resources:** the whole keyboard example app is about 29 KB of flash and 4 KB of static RAM, plus a 3 KB worker thread stack.

### On stock Device OS

The library never half-works. On a system part without the patch and no bundled patch to install, `begin()` returns `SYSTEM_ERROR_NOT_SUPPORTED`, and `status().reason` is `HidReason::NeedsPatchedDeviceOS`. The rest of your app keeps running, so firmware with `particle-hid` can safely reach devices that aren't patched yet.

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
| `statusLed(StatusLed::Rgb / StatusLed::None)`, `statusLed(pin)` | Where Bluetooth status shows: RGB LED overlays (default), a plain LED, or nowhere. |
| `installPatch(bool)` | Install the bundled patched system part when it's missing (default on). |

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
- **Pairing is gated.** Hosts can pair only inside the pairing window, which opens automatically while there are no bonds. Outside it, only bonded hosts can connect.
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

Build one with `particle compile boron examples/keyboard --target 6.6.0`. The examples don't bundle the patch, so flash it by hand first or copy an example into a project with `assetOtaDir` set.

## Repository

```text
src/                  the library
assets/               the patched system part, bundled into apps as an Asset OTA asset
examples/             example apps
device-os/            the Device OS patch, build script, and binary comparison
scripts/              flash-usb.sh and flash-cloud.sh (Boron)
tools/badge-led/      macOS CLI for the haven-badge example
test/native/          host tests for the portable core: make -C test/native
```

## License

The library is MIT. The Device OS patch is LGPL-3.0, like Device OS.
