#!/usr/bin/env bash
# Capture RTT from Board V1. There is no UART on this board (spec §1.4), so
# this is the only console.
#
#   ./rtt.sh [seconds]
#
# Uses J-Link's RTT telnet server rather than JLinkRTTLogger: the logger fails
# to locate the control block on this target even when given its exact address,
# while the telnet path finds it via SetRTTSearchRanges.
set -uo pipefail
SECS="${1:-15}"
# --reset resets the target before attaching, so boot-time output (which is
# printed once and then gone) is actually captured.
RESET=""
[ "${2:-}" = "--reset" ] && RESET="r"
JL=~/Tools/jlink/JLink_Linux_V972_x86_64/JLinkExe
CMD="$(mktemp)"
trap 'rm -f "$CMD"' EXIT
{
  echo "si SWD"; echo "speed 1000"; echo "connect"
  echo "exec SetRTTSearchRanges 0x20000000 0x80000"
  [ -n "$RESET" ] && { echo "r"; echo "g"; }
  echo "rtt start"
  echo "Sleep $(( (SECS + 8) * 1000 ))"
  echo "q"
} > "$CMD"
nohup "$JL" -device nRF54LM20A_M33 -NoGui 1 -CommandFile "$CMD" >/tmp/rttsrv.out 2>&1 &
SRV=$!
sleep 4
# stdbuf: grep block-buffers when piped, which destroys any attempt to
# timestamp lines on arrival (they all appear at once at the end).
timeout "$SECS" nc localhost 19021 | stdbuf -oL grep -av "^SEGGER J-Link\|^Process:"
wait $SRV 2>/dev/null || true
