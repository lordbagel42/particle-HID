#!/usr/bin/env bash
# Flashes a single Boron over the Particle cloud (one device, not a fleet release).
#
#   scripts/flash-cloud.sh check  <device>          platform, Device OS, product, protection, online
#   scripts/flash-cloud.sh app    <device> <bin>    user firmware built for 6.6.0
#   scripts/flash-cloud.sh system <device> [bin]    patched system-part1 (default: newest in dist/)
#   scripts/flash-cloud.sh restore <device>         official, unpatched 6.6.0 system-part1
#   scripts/flash-cloud.sh dev    <device>          mark a product device as a development device
#   scripts/flash-cloud.sh watch  <device>          stream the device's events
#
# Needs PARTICLE_ACCESS_TOKEN, curl, and jq.
#
# Order for a device not yet on 6.6.0: flash the app first. The device can't run
# it, so the cloud sends stock 6.6.0 (and a newer bootloader if needed). Then
# flash the system part; it refuses to run until the device reports 6.6.0.
#
# There is no rollback: a system part that faults at boot can only be fixed
# over USB (scripts/flash-usb.sh restore).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
API="https://api.particle.io/v1"
VERSION="6.6.0"
OFFICIAL_URL="https://github.com/particle-iot/device-os/releases/download/v$VERSION/boron-system-part1@$VERSION.bin"
OFFICIAL_SHA256="841d16feac97301e95fa460444b61132a6c92768cb866c85d45f4f56743f419b"
MAX_SIZE=573440

die() { echo "error: $*" >&2; exit 1; }
[ -n "${PARTICLE_ACCESS_TOKEN:-}" ] || die "set PARTICLE_ACCESS_TOKEN"
command -v jq >/dev/null || die "install jq"
AUTH="Authorization: Bearer $PARTICLE_ACCESS_TOKEN"

api() { # method path [curl args...]
    local method="$1" path="$2"
    shift 2
    curl -sS -X "$method" -H "$AUTH" "$@" "$API$path"
}

info() { # device -> JSON with product fields merged in when it's a product device
    local d pid
    d=$(api GET "/devices/$1")
    [ "$(jq -r '.id // empty' <<<"$d")" = "$1" ] || die "device $1: $(jq -r '.error_description // .error // .' <<<"$d")"
    pid=$(jq -r '.product_id' <<<"$d")
    if [ "$pid" != "$(jq -r '.platform_id' <<<"$d")" ]; then
        d=$(jq -s '.[0] * (.[1] | {development, firmware_version, targeted_firmware_release_version})' \
            <(echo "$d") <(api GET "/products/$pid/devices/$1"))
    fi
    echo "$d"
}

preflight() { # device [need-6.6.0]
    local d
    d=$(info "$1")
    [ "$(jq -r '.platform_id' <<<"$d")" = 13 ] || die "$1 is not a Boron (platform $(jq -r '.platform_id' <<<"$d"))"
    [ "$(jq -r '.online' <<<"$d")" = true ] || die "$1 is offline"
    [ "$(jq -r '.device_protection.status // "disabled"' <<<"$d")" = disabled ] ||
        die "$1 has Device Protection enabled; the cloud won't flash development binaries to it"
    if [ "$(jq -r '.firmware_updates_enabled' <<<"$d")" = false ] && [ "$(jq -r '.firmware_updates_forced' <<<"$d")" != true ]; then
        die "$1 has updates disabled (System.disableUpdates()); force them with: curl -X PUT $API/devices/$1 -d firmware_updates_forced=true -H 'Authorization: Bearer ...'"
    fi
    if [ "$(jq -r '.product_id' <<<"$d")" != 13 ] && [ "$(jq -r '.development' <<<"$d")" != true ]; then
        die "$1 is in product $(jq -r '.product_id' <<<"$d") and not a development device; fleet updates would replace this firmware. Run: $0 dev $1"
    fi
    if [ "${2:-}" = need-6.6.0 ] && [ "$(jq -r '.system_firmware_version' <<<"$d")" != "$VERSION" ]; then
        die "$1 reports Device OS $(jq -r '.system_firmware_version' <<<"$d"); flash an app built for $VERSION first so the cloud upgrades it"
    fi
}

verify_sha() { # file expected
    local got
    got=$(sha256sum "$1" | cut -d' ' -f1)
    [ "$got" = "$2" ] || die "$1: SHA-256 $got, expected $2"
}

flash() { # device file
    local r
    r=$(api PUT "/devices/$1" -F "file=@$2")
    echo "$r" | jq -c .
    [ "$(jq -r '.status // .message // empty' <<<"$r")" = "Update started" ] || die "flash was not accepted"
    echo "Watch progress with: $0 watch $1   (spark/flash/status, then spark/status online)"
}

[ $# -ge 2 ] || { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
CMD="$1"
DEV="$2"
case "$CMD" in
check)
    info "$DEV" | jq '{id, name, online, platform_id, system_firmware_version, product_id, development,
        firmware_version, targeted_firmware_release_version, firmware_updates_enabled,
        device_protection: (.device_protection.status // null), last_heard}'
    ;;
app)
    [ -f "${3:-}" ] || die "usage: $0 app <device> <file.bin>"
    preflight "$DEV"
    flash "$DEV" "$3"
    ;;
system)
    BIN="${3:-$(ls -t "$ROOT"/dist/boron-system-part1@$VERSION+phid.*.bin 2>/dev/null | head -1)}"
    [ -f "${BIN:-}" ] || die "no patched system part; run device-os/build.sh first"
    [ -f "$BIN.sha256" ] && verify_sha "$BIN" "$(cut -d' ' -f1 <"$BIN.sha256")"
    [ "$(wc -c <"$BIN")" -le "$MAX_SIZE" ] || die "$BIN is larger than the system-part1 region"
    preflight "$DEV" need-6.6.0
    flash "$DEV" "$BIN"
    ;;
restore)
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    curl -fsSL -o "$TMP/boron-system-part1@$VERSION.bin" "$OFFICIAL_URL"
    verify_sha "$TMP/boron-system-part1@$VERSION.bin" "$OFFICIAL_SHA256"
    preflight "$DEV" need-6.6.0
    flash "$DEV" "$TMP/boron-system-part1@$VERSION.bin"
    ;;
dev)
    PID=$(api GET "/devices/$DEV" | jq -r '.product_id')
    [ "$PID" != 13 ] || die "$DEV is not in a product"
    api PUT "/products/$PID/devices/$DEV" -d development=true | jq -c .
    ;;
watch)
    curl -sSN -H "$AUTH" "$API/devices/$DEV/events" | while IFS= read -r line; do
        case "$line" in
        event:*) ev="${line#event: }" ;;
        data:*) printf '%s %-20s %s\n' "$(date -u +%H:%M:%S)" "$ev" "$(jq -r '.data' <<<"${line#data: }")" ;;
        esac
    done
    ;;
*) die "unknown command $CMD" ;;
esac
