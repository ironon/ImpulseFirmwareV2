#!/usr/bin/env python3
"""Summarise a battery_log.sh capture: rate, span, and reset detection."""
import re, sys, statistics as st

rows = []
for line in open(sys.argv[1], errors="ignore"):
    m = re.search(r"uptime_ms=(\d+).*state=(\d+).*vbat=(\d+) mV", line)
    if m:
        rows.append((int(m.group(1)) / 3600000.0, int(m.group(2)), int(m.group(3))))
if len(rows) < 5:
    print(f"only {len(rows)} samples"); sys.exit(1)

# A reset shows up as uptime going BACKWARDS; report it rather than fitting
# across it, because a reboot mid-run invalidates a single linear rate.
resets = sum(1 for a, b in zip(rows, rows[1:]) if b[0] < a[0])
t = [r[0] for r in rows]; v = [r[2] for r in rows]
states = {s for _, s, _ in rows}
span = t[-1] - t[0]
print(f"samples={len(rows)}  span={span:.2f} h  resets={resets}  states_seen={sorted(states)}")
print(f"vbat {v[0]} -> {v[-1]} mV   delta={v[-1]-v[0]} mV")
if span > 0.05:
    print(f"rate = {(v[-1]-v[0])/span:+.1f} mV/hour")
    r = (v[-1]-v[0])/span
    if r < -1:
        print(f"linear headroom from {v[-1]} mV to 3300 mV: {(v[-1]-3300)/-r:.1f} h "
              f"(OPTIMISTIC — LiPo falls off below ~3.7 V)")
