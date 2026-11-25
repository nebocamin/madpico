#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Builds the firmware, puts the Pico into the bootloader via the 1200 baud
# reset trick, copies the UF2 and waits for the serial port to come back.
# If the Pico doesn't respond (e.g. crashed firmware), plug it in while
# holding BOOTSEL and run this script again.
#
# Environment: ENV (PlatformIO environment, default pico2w), PIO (pio binary),
#              PORT (serial port), MNT (RP2350 mount point)
set -euo pipefail
cd "$(dirname "$0")/.."

PIO=${PIO:-$(command -v pio || echo "$HOME/.local/bin/pio")}
PY=$(dirname "$(readlink -f "$PIO")")/python
PORT=${PORT:-/dev/ttyACM0}
MNT=${MNT:-/run/media/$USER/RP2350}
ENV=${ENV:-pico2w}

"$PIO" run -e "$ENV"

if [ ! -e "$MNT/INFO_UF2.TXT" ]; then
	if [ -e "$PORT" ]; then
		"$PY" -c "import serial,time; s=serial.Serial('$PORT',1200); s.dtr=False; time.sleep(0.2); s.close()" 2>/dev/null || true
	fi
	for _ in $(seq 1 15); do [ -e "$MNT/INFO_UF2.TXT" ] && break; sleep 1; done
fi
[ -e "$MNT/INFO_UF2.TXT" ] || { echo "Pico is not in the bootloader - plug it in while holding BOOTSEL"; exit 1; }

cp ".pio/build/$ENV/firmware.uf2" "$MNT/" && sync
for _ in $(seq 1 20); do [ -e "$PORT" ] && break; sleep 1; done
echo "Flashed, serial port: $PORT"
