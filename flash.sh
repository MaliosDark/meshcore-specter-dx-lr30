#!/usr/bin/env bash
# flash.sh, flash SPECTER on DX-LR30 (STM32F103C8T6)
#
# Method: bootloader via physical buttons (more reliable than DTR/RTS on CH340)
#
# Usage:
#   ./flash.sh              -> build + flash
#   ./flash.sh --no-build   -> flash the already-built .bin only
#   ./flash.sh --monitor    -> flash and open serial monitor

set -e

PORT="/dev/ttyUSB0"
BAUD=57600  # This board requires 57600, not 115200
FW=".pio/build/specter_repeater/firmware.bin"
BUILD=true
MONITOR=false

for arg in "$@"; do
    case $arg in
        --no-build)  BUILD=false ;;
        --monitor)   MONITOR=true ;;
        --help|-h)
            grep '^#' "$0" | head -15 | sed 's/^# \?//'
            exit 0 ;;
    esac
done

# -- Build --------------------------------------------------------------------
if $BUILD; then
    echo "════════════════════════════════════════"
    echo " Building SPECTER..."
    echo "════════════════════════════════════════"
    pio run -e specter_repeater
    echo ""
fi

# -- Verify firmware ----------------------------------------------------------
if [[ ! -f "$FW" ]]; then
    echo " ERROR: $FW was not found"
    echo "Run first: pio run -e specter_repeater"
    exit 1
fi

SIZE=$(stat -c%s "$FW")
echo "Firmware: $FW ($SIZE bytes)"
echo ""

# -- Bootloader instructions --------------------------------------------------
echo "════════════════════════════════════════"
echo " BOOTLOADER ENTRY (manual mode)"
echo "════════════════════════════════════════"
echo ""
echo "  1. Hold       [BOOT0]"
echo "  2. Press/release [RESET]"
echo "  3. Release    [BOOT0]"
echo ""
echo "Then press ENTER to start flashing..."
read -r

# -- Flash --------------------------------------------------------------------
echo "Flashing -> $PORT @ ${BAUD}..."
stm32flash \
    -w "$FW" \
    -v \
    -R \
    -b "$BAUD" \
    "$PORT"

echo ""
echo "════════════════════════════════════════"
echo " SPECTER flashed successfully"
echo "════════════════════════════════════════"

# -- Serial monitor (optional) ------------------------------------------------
if $MONITOR; then
    echo ""
    echo "Opening serial monitor (Ctrl+] to exit)..."
    sleep 1
    python3 -m serial.tools.miniterm --eol LF "$PORT" "$BAUD"
fi
