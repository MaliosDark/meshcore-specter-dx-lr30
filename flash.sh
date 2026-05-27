#!/usr/bin/env bash
# flash.sh, Flashea SPECTER en DX-LR30 (STM32F103C8T6)
#
# Método: bootloader via botones físicos (más fiable que DTR/RTS en CH340)
#
# Uso:
#   ./flash.sh              → compila + flashea
#   ./flash.sh --no-build   → solo flashea el .bin ya compilado
#   ./flash.sh --monitor    → flashea y abre monitor serie

set -e

PORT="/dev/ttyUSB0"
BAUD=57600  # ⚠️ Este board requiere 57600, no 115200
FW=".pio/build/specter/firmware.bin"
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

# ── Compilar ──────────────────────────────────────────────────────────────────
if $BUILD; then
    echo "════════════════════════════════════════"
    echo " Compilando SPECTER..."
    echo "════════════════════════════════════════"
    pio run -e specter
    echo ""
fi

# ── Verificar firmware ────────────────────────────────────────────────────────
if [[ ! -f "$FW" ]]; then
    echo " ERROR: No se encontró $FW"
    echo "Ejecuta primero: pio run -e specter"
    exit 1
fi

SIZE=$(stat -c%s "$FW")
echo "Firmware: $FW ($SIZE bytes)"
echo ""

# ── Instrucciones de bootloader ───────────────────────────────────────────────
echo "════════════════════════════════════════"
echo " ENTRADA AL BOOTLOADER (modo manual)"
echo "════════════════════════════════════════"
echo ""
echo "  1. Mantén pulsado  [BOOT0]"
echo "  2. Pulsa y suelta  [RESET]"
echo "  3. Suelta          [BOOT0]"
echo ""
echo "Luego presiona ENTER para iniciar el flash..."
read -r

# ── Flashear ──────────────────────────────────────────────────────────────────
echo "Flasheando → $PORT @ ${BAUD}..."
stm32flash \
    -w "$FW" \
    -v \
    -R \
    -b "$BAUD" \
    "$PORT"

echo ""
echo "════════════════════════════════════════"
echo " SPECTER flasheado correctamente ✓"
echo "════════════════════════════════════════"

# ── Monitor serie (opcional) ──────────────────────────────────────────────────
if $MONITOR; then
    echo ""
    echo "Abriendo monitor serie (Ctrl+] para salir)..."
    sleep 1
    python3 -m serial.tools.miniterm --eol LF "$PORT" "$BAUD"
fi
