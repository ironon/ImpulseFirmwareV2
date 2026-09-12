#!/usr/bin/env bash
# Long-run battery logger.
#
#   ./battery_log.sh <output-file> [hours]
#
# Appends every heartbeat line to <output-file>, forever, by running rtt.sh in
# short slices and reconnecting between them.
#
# WHY SLICES: a single long rtt.sh has repeatedly truncated early (a 200 s
# request once yielded 54 s). Losing a multi-hour capture to that is
# unacceptable, and each heartbeat carries its own uptime and vbat, so a gap
# between slices costs nothing — the series is reconstructed from the line
# contents, not from continuity of the stream.
set -uo pipefail
OUT="${1:?usage: battery_log.sh <output-file> [hours]}"
HOURS="${2:-12}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
END=$(( $(date +%s) + $(python3 -c "print(int(float('$HOURS')*3600))") ))
: > "$OUT"
while [ "$(date +%s)" -lt "$END" ]; do
  timeout 260 "$HERE/rtt.sh" 200 2>/dev/null \
    | tr -d '\000' | grep -a "alive uptime_ms=" >> "$OUT"
  # Let J-Link fully release the probe before the next slice re-attaches.
  pkill -f "nc localhost 19021" 2>/dev/null
  pkill -f JLinkExe 2>/dev/null
  sleep 5
done
