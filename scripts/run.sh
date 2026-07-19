#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR/app"

BOARD="thingy91/nrf9160/ns"
MERGED_HEX="$PROJECT_DIR/app/build/app/zephyr/tfm_merged.hex"
SERIAL="260114597"
DEVICE="nRF9160_xxAA"

usage() {
    cat <<EOF
Usage: $(basename "$0") [option]

Options:
  build       Compile the firmware (west build --pristine)
  flash       Flash the merged TF-M + app image (no rebuild)
  reset       Reboot the device via J-Link
  check       Check if device is in HardFault via J-Link
  rtt         Start RTT monitor (console output via pylink)
  rtt viewer  JLinkRTTViewer with correct RTT address
  rtt client  Start RTT monitor (console output via pylink)
  rtt addr    Print RTT control block address
  all         Build + flash + RTT client
  (default)   Build (pristine) + flash + RTT client
  -h, --help  Show this help
EOF
    exit 0
}

rtt_addr() {
    local map="$PROJECT_DIR/app/build/app/zephyr/zephyr.map"
    grep "_SEGGER_RTT" "$map" 2>/dev/null | awk '{print $1}' || {
        echo "Error: _SEGGER_RTT not found. Run build first." >&2
        exit 1
    }
}

do_rtt() {
    local cmd="${1:-auto}"
    case "$cmd" in
        viewer)
            # GUI
            JLinkRTTViewer --device "$DEVICE" --interface SWD --speed 4000 --rttaddr "$(rtt_addr)" &
            ;;
        client|auto|"")
            # Console monitor via pylink
            "$PROJECT_DIR/scripts/rtt_monitor.py" \
                --elf "$PROJECT_DIR/app/build/app/zephyr/zephyr.elf" \
                --snr "$SERIAL"
            ;;
        addr)
            rtt_addr
            ;;
    esac
}

check_hardfault() {
    echo "=== Checking CPU state via J-Link ==="
    local output
    output=$(echo "connect
halt
exit" | JLinkExe -device nRF9160_xxAA -if swd -speed 4000 2>&1)

    # Display the full tool output before interpreting
    echo "--- Full JLinkExe output ---"
    echo "$output"
    echo "--- End of JLinkExe output ---"

    if echo "$output" | grep -q "HardFault"; then
        echo "!!! HARDFAULT DETECTED !!!"
        echo "$output" | grep -E "PC|XPSR|IPSR|R[0-9]"
        return 1
    else
        local pc
        pc=$(echo "$output" | grep "PC" | awk '{print $2}')
        echo "OK — PC = $pc, no HardFault"
        return 0
    fi
}

do_build() {
    local pristine="${1:+--pristine}"
    echo "=== Building for $BOARD${pristine:+ (pristine)} ==="
    west build -b "$BOARD" $pristine
    echo "Build OK"
}

do_flash() {
    if [ ! -f "$MERGED_HEX" ]; then
        echo "ERROR: $MERGED_HEX not found. Run build first." >&2
        exit 1
    fi

    local max_attempts=3
    local attempt=1

    while [ "$attempt" -le "$max_attempts" ]; do
        echo "=== Flashing (attempt $attempt/$max_attempts) ==="
        if west flash --erase --no-rebuild --hex-file "$MERGED_HEX"; then
            echo "Flash OK"
            return 0
        fi

        if [ "$attempt" -lt "$max_attempts" ]; then
            echo ""
            echo "⚠️  Flash failed. Is another J-Link application (RTT Viewer, RTT Client,"
            echo "   JLinkExe, etc.) currently attached to the device?"
            read -r -p "   Close it and press Enter to retry (or 's' to skip remaining attempts)... " response
            if [ "$response" = "s" ] || [ "$response" = "S" ]; then
                echo "Skipping remaining attempts."
                break
            fi
        fi
        attempt=$((attempt + 1))
    done

    echo "ERROR: Flash failed after $max_attempts attempts" >&2
    exit 1
}

do_reset() {
    echo "=== Rebooting device via J-Link ==="
    echo "connect
exit" | JLinkExe -device nRF9160_xxAA -if swd -speed 4000 2>&1 | grep -E "Connected|O\.K\." || true
    echo "Reset OK"
}

case "${1:-}" in
    build)
        do_build
        ;;
    flash)
        do_flash
        ;;
    check)
        check_hardfault
        ;;
    reset)
        do_reset
        ;;
    rtt)
        shift 2>/dev/null || true
        do_rtt "$@"
        ;;
    "")
        do_build pristine
        do_flash
        do_rtt client
        ;;
    all)
        do_build
        do_flash
        do_rtt client
        ;;
    -h|--help)
        usage
        ;;
    *)
        echo "Unknown option: $1"
        usage
        ;;
esac
