#!/usr/bin/env bash
# Flashes a Boron over USB with the Particle CLI.
#
#   scripts/flash-usb.sh system [file.bin]   # patched system-part1 (default: newest in dist/)
#   scripts/flash-usb.sh app <file.bin>      # user firmware built for 6.6.0
#   scripts/flash-usb.sh restore             # official, unpatched 6.6.0 system-part1
#   scripts/flash-usb.sh check               # Device OS version on the connected device
#
# USB only on purpose: Gen 3 has no system-part rollback, and a system part
# that faults at boot needs a cable to recover. Keep `restore` within reach.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLATFORM="boron"
VERSION="6.6.0"
OFFICIAL_URL="https://github.com/particle-iot/device-os/releases/download/v$VERSION/$PLATFORM-system-part1@$VERSION.bin"
OFFICIAL_SHA256="841d16feac97301e95fa460444b61132a6c92768cb866c85d45f4f56743f419b"
MAX_SIZE=573440

die() { echo "error: $*" >&2; exit 1; }
command -v particle >/dev/null || die "install the Particle CLI: https://docs.particle.io/getting-started/developer-tools/cli/"

one_device() {
    local n
    n=$(particle usb list 2>/dev/null | grep -ci "$PLATFORM" || true)
    [ "$n" -eq 1 ] || die "connect exactly one $PLATFORM over USB (found $n)"
}

verify_sha() { # file expected
    local got
    got=$(sha256sum "$1" | cut -d' ' -f1)
    [ "$got" = "$2" ] || die "$1: SHA-256 $got, expected $2"
}

flash_system() { # file
    local size
    size=$(wc -c <"$1")
    [ "$size" -le "$MAX_SIZE" ] || die "$1 is $size bytes; system-part1 holds $MAX_SIZE"
    one_device
    particle usb dfu
    particle flash --usb "$1"
    echo "Flashed $1. The device restarts; wait for it to breathe before flashing the app."
}

case "${1:-}" in
system)
    BIN="${2:-$(ls -t "$ROOT"/dist/$PLATFORM-system-part1@$VERSION+phid.*.bin 2>/dev/null | head -1)}"
    [ -f "${BIN:-}" ] || die "no patched system part; run device-os/build.sh first"
    if [ -f "$BIN.sha256" ]; then
        verify_sha "$BIN" "$(cut -d' ' -f1 <"$BIN.sha256")"
    fi
    flash_system "$BIN"
    ;;
app)
    [ -f "${2:-}" ] || die "usage: $0 app <file.bin>"
    one_device
    particle flash --usb "$2"
    ;;
restore)
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    curl -fsSL -o "$TMP/system-part1.bin" "$OFFICIAL_URL"
    verify_sha "$TMP/system-part1.bin" "$OFFICIAL_SHA256"
    flash_system "$TMP/system-part1.bin"
    ;;
check)
    one_device
    particle usb list
    particle serial inspect 2>/dev/null || echo "(put the device in listening mode for 'particle serial inspect' details)"
    ;;
*)
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
