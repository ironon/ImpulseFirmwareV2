#!/usr/bin/env bash
# Continuous RTT capture, keeping EVERY line.
#
#   ./rtt_watch.sh <output-file> [hours]
#
# Runs rtt.sh in short slices and reconnects between them. The point is to keep
# a host attached AT ALL TIMES: the SEGGER RTT up-buffer is in skip mode, so
# once it fills every subsequent write is silently dropped — which has now cost
# three separate diagnoses, because a full buffer looks exactly like a crashed
# board. A reader attached continuously keeps it drained.
set -uo pipefail
OUT="${1:?usage: rtt_watch.sh <output-file> [hours]}"
HOURS="${2:-2}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
END=$(( $(date +%s) + ${HOURS%.*} * 3600 ))
: > "$OUT"
while [ "$(date +%s)" -lt "$END" ]; do
  timeout 260 "$HERE/rtt.sh" 200 2>/dev/null \
    | tr -d '\000' | sed 's/\x1b\[[0-9;]*m//g' >> "$OUT"
  pkill -f "nc localhost 19021" 2>/dev/null
  pkill -f JLinkExe 2>/dev/null
  sleep 3
done
