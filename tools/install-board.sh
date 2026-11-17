#!/usr/bin/env bash
# Installs the custom "Seeed Wio Tracker L1 / L1 Pro" board definition into the
# Arduino sketchbook (Linux and macOS).
#
# Prerequisite: the "Seeed nRF52 Boards" package (Seeeduino:nrf52) is installed via
# the Arduino IDE Boards Manager. This board definition reuses its core, libraries,
# tools and bootloader files through symbolic links.
#
# Usage:  tools/install-board.sh [path-to-sketchbook]
#         (default sketchbook: ~/Arduino on Linux, ~/Documents/Arduino on macOS)

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO_DIR/board/WioTrackerL1/nrf52"

case "$(uname -s)" in
  Darwin) ARDUINO15="$HOME/Library/Arduino15"; DEFAULT_SKETCHBOOK="$HOME/Documents/Arduino" ;;
  *)      ARDUINO15="$HOME/.arduino15";        DEFAULT_SKETCHBOOK="$HOME/Arduino" ;;
esac
SKETCHBOOK="${1:-$DEFAULT_SKETCHBOOK}"

# Find the installed Seeed nRF52 package (newest version if several are installed)
SEEED_BASE="$ARDUINO15/packages/Seeeduino/hardware/nrf52"
if [ ! -d "$SEEED_BASE" ]; then
  echo "ERROR: Seeed nRF52 package not found in $SEEED_BASE"
  echo "Install 'Seeed nRF52 Boards' in the Arduino IDE Boards Manager first (see README)."
  exit 1
fi
SEEED="$(ls -d "$SEEED_BASE"/*/ | sort -V | tail -n 1)"
SEEED="${SEEED%/}"
echo "Using Seeed nRF52 package: $SEEED"
case "$(basename "$SEEED")" in
  1.1.13) ;;
  *) echo "NOTE: tested with version 1.1.13 only. If the build fails, install 1.1.13." ;;
esac

DEST="$SKETCHBOOK/hardware/WioTrackerL1/nrf52"
echo "Installing board definition to: $DEST"
mkdir -p "$DEST/variants"

cp "$SRC/boards.txt" "$SRC/platform.txt" "$DEST/"
mkdir -p "$DEST/variants/WioTrackerL1"
cp "$SRC/variants/WioTrackerL1/variant.h" "$SRC/variants/WioTrackerL1/variant.cpp" "$DEST/variants/WioTrackerL1/"

# platform.txt refers to {runtime.platform.path}/libraries, /tools and /bootloader,
# which must point into the Seeed package
for d in libraries tools bootloader; do
  ln -sfn "$SEEED/$d" "$DEST/$d"
done

echo
echo "Done. Restart the Arduino IDE and select:"
echo "  Tools > Board > Seeed Wio Tracker L1 / L1 Pro (eigene Definition)"
echo "Re-run this script after updating the Seeed nRF52 package."
