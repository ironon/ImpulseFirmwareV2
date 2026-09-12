#!/usr/bin/env bash
# Capture N seconds of live CS estimates from the watch and summarise them.
#
#   ./cs_sample.sh [seconds] [label]
#
# Used for ground-truth calibration: park the two boards a known distance
# apart, run this, and record the median against the true separation. The
# median (not the mean) is what the firmware's burst logic uses.
set -uo pipefail
SECS="${1:-25}"
LABEL="${2:-unlabelled}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
timeout $((SECS + 60)) "$HERE/rtt.sh" "$SECS" 2>&1 | tr -d '\000' > "$OUT"
grep -aoE "ifft: [0-9.]+, phase_slope: [0-9.]+, rtt: [0-9.]+" "$OUT" \
  | sed 's/[a-z_]*: //g' | tr -d ',' > "$OUT.est"
python3 - "$OUT.est" "$LABEL" <<'PY'
import sys, statistics as st
rows=[list(map(float,l.split())) for l in open(sys.argv[1]) if l.strip()]
if not rows:
    print(f"{sys.argv[2]}: NO ESTIMATES — is the anchor advertising and in range?")
    raise SystemExit(1)
i=[r[0] for r in rows]; p=[r[1] for r in rows]
print(f"{sys.argv[2]}: n={len(rows)}  "
      f"ifft median={st.median(i):.3f} sd={st.pstdev(i):.3f}  "
      f"phase_slope median={st.median(p):.3f}  "
      f"divergence={st.median(p)-st.median(i):+.3f} m")
PY
