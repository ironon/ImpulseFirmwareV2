#!/usr/bin/env bash
# Flash a role image to Board V1 over SWD.
#
#   ./flash.sh watch     # or: anchor
#
# Verifies the die before writing — J-Link will happily "connect" to the wrong
# part, since both this and the nRF54L15-DK are Cortex-M33 and the device name
# only selects a flash algorithm. FICR RAM size discriminates: 512 KB is the
# nRF54LM20A on Board V1, 256 KB is the DK.
set -euo pipefail
ROLE="${1:-watch}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JL=~/Tools/jlink/JLink_Linux_V972_x86_64/JLinkExe
HEX="$HERE/build-$ROLE/nrf_firmware/zephyr/zephyr.hex"

case "$ROLE" in watch|anchor) ;; *) echo "usage: $0 {watch|anchor}"; exit 2 ;; esac
[ -f "$HEX" ] || { echo "missing $HEX — run ./build.sh $ROLE first"; exit 1; }

CMD="$(mktemp)"; trap 'rm -f "$CMD"' EXIT
{ echo "si SWD"; echo "speed 1000"; echo "connect"; echo "mem32 0x00FFC328, 1"; echo "q"; } > "$CMD"
RAM="$("$JL" -device nRF54LM20A_M33 -NoGui 1 -CommandFile "$CMD" 2>&1 \
      | grep -oE '^00FFC328 = [0-9A-F]+' | awk '{print $3}' || true)"

if [ -z "${RAM:-}" ]; then
  echo "No SWD connection. Seat the TC2030 firmly on J2 — a partial seat gives"
  echo "a healthy 3.3 V VTref with no SWD response at all."
  exit 1
fi
if [ "$((16#$RAM))" -ne 512 ]; then
  echo "REFUSING: connected die reports $((16#$RAM)) KB RAM, expected 512 (nRF54LM20A)."
  echo "That is the DK, not Board V1."
  exit 1
fi

echo "target confirmed: Board V1. Flashing $ROLE..."
# "r" before "halt" is load-bearing. With a bare "halt" the running image can
# refuse to stop ("Failed to halt CPU") and the write then fails verification
# at 0x00040000 — which looks like a flash-size or RRAM fault and is not one.
# Resetting into halt first fixes it WITHOUT an erase, so NVS survives.
# [verified 2026-09-10]
{ echo "si SWD"; echo "speed 4000"; echo "connect"; echo "r"; echo "halt"
  echo "loadfile $HEX"; echo "r"; echo "g"; echo "q"; } > "$CMD"
"$JL" -device nRF54LM20A_M33 -NoGui 1 -CommandFile "$CMD" 2>&1 | grep -E "Verifying|O.K.|Error"
