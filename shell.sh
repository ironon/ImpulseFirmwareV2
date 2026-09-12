#!/usr/bin/env bash
# Send bring-up shell commands over RTT and capture the reply.
#   ./shell.sh "impulse led 40 0 0" ["second cmd" ...]
#
# Board V1 has no UART; this drives the shell on the RTT down-channel.
#
# Shell and logs BOTH live on RTT channel 0 (port 19021): the shell renders
# carries the log backend, and shell_rtt.c BUILD_ASSERTs that the two are not
# on the same channel. Reading 19021 here returns logs and never a prompt,
# which looks exactly like a shell that is not running. [verified 2026-09-10]
set -uo pipefail
JL=~/Tools/jlink/JLink_Linux_V972_x86_64/JLinkExe
PORT=19021
CMD="$(mktemp)"; trap 'rm -f "$CMD"' EXIT
{ echo "si SWD"; echo "speed 4000"; echo "connect"
  echo "exec SetRTTSearchRanges 0x20000000 0x80000"
  echo "rtt start"; echo "Sleep 60000"; echo "q"; } > "$CMD"
nohup "$JL" -device nRF54LM20A_M33 -NoGui 1 -CommandFile "$CMD" >/tmp/rttsrv.out 2>&1 &
sleep 4
{
  # The first thing written after nc connects is reliably lost — the RTT
  # down-channel is not ready yet. Prime with a bare newline and wait.
  sleep 2; printf '\r\n'; sleep 2
  for c in "$@"; do printf '%s\r\n' "$c"; sleep 5; done
  sleep 3
} | timeout 50 nc localhost "$PORT" | tr -d '\000' | stdbuf -oL grep -av "^SEGGER J-Link\|^Process:"
