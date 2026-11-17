#!/usr/bin/env bash
# Builds the firmware with arduino-cli and converts it to a UF2 file that can be
# dragged onto the tracker's bootloader drive.
#
# Requires: arduino-cli in PATH (or ARDUINO_CLI=/path/to/arduino-cli), python3,
#           the Seeed nRF52 package, the libraries U8g2 + TinyGPSPlus and the board
#           definition installed with tools/install-board.sh.
#
# Usage:  tools/build-uf2.sh            -> build/CanonGPS_L1.uf2

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI="${ARDUINO_CLI:-arduino-cli}"
OUT="$REPO_DIR/build"
FQBN="WioTrackerL1:nrf52:wiotrackerl1"

case "$(uname -s)" in
  Darwin) ARDUINO15="$HOME/Library/Arduino15" ;;
  *)      ARDUINO15="$HOME/.arduino15" ;;
esac
UF2CONV="$(ls -d "$ARDUINO15"/packages/Seeeduino/hardware/nrf52/*/tools/uf2conv/uf2conv.py | sort -V | tail -n 1)"

mkdir -p "$OUT"
"$CLI" compile --fqbn "$FQBN" --output-dir "$OUT" "$REPO_DIR/firmware/CanonGPS_L1"

# Family ID 0xADA52840 = nRF52840 (Adafruit UF2 bootloader)
python3 "$UF2CONV" -f 0xADA52840 -c -o "$OUT/CanonGPS_L1.uf2" "$OUT/CanonGPS_L1.ino.hex"
echo "UF2 written to $OUT/CanonGPS_L1.uf2"
