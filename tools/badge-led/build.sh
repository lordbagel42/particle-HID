#!/usr/bin/env bash
# Builds ./badge-led for macOS (needs the Xcode command line tools: xcode-select --install).
# The Info.plist is embedded so macOS can ask for Bluetooth permission.
#
# Command Line Tools updates can leave a default SDK built for a newer Swift than
# the installed compiler. If the default SDK fails, try the other installed SDKs,
# newest first. Set SDKROOT to choose one yourself.
set -euo pipefail
cd "$(dirname "$0")"

build() {
    swiftc -O -o badge-led badge-led.swift \
        -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker Info.plist
}

log="$(mktemp)"
trap 'rm -f "$log"' EXIT
if [ -n "${SDKROOT:-}" ]; then
    build
elif ! build 2>"$log"; then
    built=""
    for sdk in $(ls -d "$(dirname "$(xcrun --show-sdk-path)")"/MacOSX[0-9]*.sdk 2>/dev/null | sort -rV); do
        if SDKROOT="$sdk" build 2>/dev/null; then
            built="$sdk"
            break
        fi
    done
    if [ -z "$built" ]; then
        cat "$log" >&2
        echo "badge-led: no installed SDK works with $(swiftc --version 2>&1 | head -1)" >&2
        exit 1
    fi
    echo "Default SDK doesn't match the Swift compiler; used $built"
fi
echo "Built $(pwd)/badge-led"
