#!/usr/bin/env bash
# Builds the patched system-part1 (PHID ABI) from Device OS source.
#
#   device-os/build.sh [platform]        # default: boron
#
# Needs git, make, and GNU Arm Embedded Toolchain 10-2020-q4-major
# (arm-none-eabi-gcc 10.2.1) on PATH, the version Device OS 6.6.0 pins.
# Output: dist/<platform>-system-part1@6.6.0+phid.<N>.bin and a .sha256 file.
set -euo pipefail

PLATFORM="${1:-boron}"
TAG="v6.6.0"
PHID_REV="1" # bump when the patch series changes
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${DEVICE_OS_DIR:-$ROOT/.build/device-os}"
DIST="$ROOT/dist"
# system-part1 must fit its flash region; a larger image would overwrite the user part.
MAX_SIZE=573440

die() { echo "error: $*" >&2; exit 1; }

command -v arm-none-eabi-gcc >/dev/null || die "arm-none-eabi-gcc not on PATH (need 10-2020-q4-major)"
arm-none-eabi-gcc --version | head -1 | grep -q "10.2.1" ||
    die "need arm-none-eabi-gcc 10.2.1 (10-2020-q4-major); found: $(arm-none-eabi-gcc --version | head -1)"

if [ ! -d "$WORK/.git" ]; then
    echo "Cloning Device OS $TAG into $WORK"
    git clone --quiet --depth 1 --branch "$TAG" --recurse-submodules --shallow-submodules \
        https://github.com/particle-iot/device-os.git "$WORK"
fi

cd "$WORK"
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null || die "$WORK has no tag $TAG; remove it or set DEVICE_OS_DIR"

# Apply the patch series onto the tag itself, so a rerun never stacks patches.
git -c advice.detachedHead=false checkout --quiet --force "$TAG"
git submodule update --quiet --recursive
git -c user.name=particle-hid -c user.email=particle-hid@users.noreply.github.com \
    am --quiet --committer-date-is-author-date "$ROOT"/device-os/patches/*.patch

MODULE="modules/$PLATFORM/system-part1"
[ -d "$MODULE" ] || die "no $MODULE in Device OS $TAG"
echo "Building $PLATFORM system-part1"
# Device OS make doesn't relink when only a static library changed; force it.
rm -rf build/target/system-part1
make -s -C "$MODULE" PLATFORM="$PLATFORM" -j"$(nproc 2>/dev/null || echo 4)" all

BIN="$(find build/target/system-part1 -path "*platform-*" -name system-part1.bin | head -1)"
[ -f "$BIN" ] || die "build produced no system-part1.bin"
SIZE=$(wc -c <"$BIN")
[ "$SIZE" -le "$MAX_SIZE" ] || die "system-part1 is $SIZE bytes; the region holds $MAX_SIZE"

mkdir -p "$DIST"
OUT="$DIST/$PLATFORM-system-part1@${TAG#v}+phid.$PHID_REV.bin"
cp "$BIN" "$OUT"
(cd "$DIST" && sha256sum "$(basename "$OUT")" >"$(basename "$OUT").sha256")
echo "Built $OUT ($SIZE of $MAX_SIZE bytes)"
cat "$OUT.sha256"
