#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export BOARD_TARGET="shadow/nrf9160/ns"
export JLINK_SNR="260114597"
export JLINK_DEVICE="nRF9160_xxAA"
export BOARD_ROOT="$SCRIPT_DIR/../app"

exec "$SCRIPT_DIR/run.sh" "$@"
