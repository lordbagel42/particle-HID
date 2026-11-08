# The Device OS patch ("PHID ABI")

`particle-hid` needs one change to Device OS: a small hook in the nRF52840 BLE HAL that lets user firmware handle bonding. Stock Device OS never stores pairing keys, never answers a returning host's encryption request, and forgets subscriptions, so hosts treat a HID device as new every time, or drop it.

- **Source:** [`lordbagel42/device-os`, branch `phid/v6.6.0`](https://github.com/lordbagel42/device-os/tree/phid/v6.6.0). It is one commit on top of `v6.6.0`. The same commit is in [`patches/`](patches/) as a `git am` patch.
- **Binary:** `boron-system-part1@6.6.0+phid.1.bin`, attached to each [particle-hid release](https://github.com/lordbagel42/particle-hid/releases). Check it against the `.sha256` file next to it.
- **License:** the patch is part of Device OS and is LGPL-3.0, like the rest of Device OS.

## What it changes

| | |
| --- | --- |
| Files | `hal/src/nRF52840/ble_hal.cpp` (+98 lines), new `hal/inc/ble_hal_phid.h` |
| Binary | +384 bytes; 4,696 bytes left in the 573,440-byte system-part1 region |
| Functions | 4 changed (`processBleEventFromThread`, `processSecurityEventFromThread`, `processGattServerEvents`, `hal_ble_internal`), 2 added |
| Unchanged | bootloader, SoftDevice and its configuration, RAM layout, all 33 dynalib tables (820 entries), module version (6600) and dependencies |

Without the library, the patched part behaves exactly like stock: every new branch checks for an installed delegate. Apps compiled for stock 6.6.0, including cloud compiles, run on it unchanged, and apps that use `particle-hid` still run on stock Device OS (`BleHid.begin()` returns an error instead).

The hook is exposed through `hal_ble_internal()`, an existing dynalib export that stock nRF52840 Device OS answers with `SYSTEM_ERROR_NOT_SUPPORTED`:

- `PHID_OP_GET_INFO` reports the ABI version and capabilities. This is how the library detects the patch.
- `PHID_OP_SET_DELEGATE` installs a callback that runs on the BLE event thread. It decides whether to bond and where the keys go (`SEC_PARAMS_REQUEST`), answers `SEC_INFO_REQUEST` and `SYS_ATTR_MISSING`, sees every other BLE event including `SC_CONFIRM`, and gets a wake semaphore on `HVN_TX_COMPLETE`.

The full contract is in [`ble_hal_phid.h`](../src/phid/ble_hal_phid.h).

## Install it on a device

The device must already run Device OS **6.6.0**, including its bootloader. Check with `particle usb list` or `scripts/flash-cloud.sh check <device>`.

**Over USB** (recommended for the first device):

```sh
particle update                                         # only if the device isn't on 6.6.0 yet
particle flash --usb boron-system-part1@6.6.0+phid.1.bin
```

**Over the cloud**, one device at a time:

```sh
export PARTICLE_ACCESS_TOKEN=...
scripts/flash-cloud.sh check  <device>
scripts/flash-cloud.sh app    <device> your-app.bin     # built for 6.6.0; the cloud upgrades an older device to stock 6.6.0
scripts/flash-cloud.sh system <device>                  # then the patched system part
```

A product device must be marked as a development device first (`scripts/flash-cloud.sh dev <device>`). Otherwise the product's fleet updates replace the test firmware.

**Back to stock:** `scripts/flash-usb.sh restore` or `scripts/flash-cloud.sh restore <device>`. Both download Particle's official 6.6.0 system part and check its SHA-256 before flashing.

**Things to know:**

- Gen 3 devices have no system-part rollback. If a system part faults at boot, only USB can recover the device, so keep one within reach of a cable the first time.
- Anything that sends the device a stock system part replaces the patch, for example `particle update` or a product's Device OS target. `BleHid.status().reason` then reports `NeedsPatchedDeviceOS`. Bonds stay on flash, so re-flashing the patched part restores them.

## Build it yourself

You need git, make, and GNU Arm Embedded Toolchain **10-2020-q4-major** (`arm-none-eabi-gcc` 10.2.1) on `PATH`:

```sh
device-os/build.sh boron     # other nRF52840 platforms with a modules/<platform> directory (argon, b5som) should build too; untested
```

The script clones Device OS `v6.6.0` into `.build/device-os`, applies `patches/*.patch` with `git am`, builds `system-part1`, and checks that it fits its flash region. It writes `dist/<platform>-system-part1@6.6.0+phid.1.bin` and a `.sha256`. Only the Boron build has been tested on hardware.

To confirm that a build changes only the intended code, build stock `v6.6.0` too and compare the two ELF files:

```sh
device-os/compare-to-stock.py stock/system-part1.elf patched/system-part1.elf
```

The script compares every function with layout shifts masked out, and every dynalib entry. For this patch it should report only the 4 changed functions, the 2 added ones, and no dynalib differences. A stock 6.6.0 built this way differs from Particle's official binary only in its build ID, SHA-256, and CRC.

## Porting to another Device OS version

Rebase the commit onto the new tag, rebuild, and run `compare-to-stock.py` against a stock build of the same tag. Then update `TAG` in `build.sh` and the version checks in the `scripts/` flash scripts. The library checks the ABI version and capabilities at runtime, not the Device OS version.
