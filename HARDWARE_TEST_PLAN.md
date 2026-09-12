# Hardware test plan — nRF port

Everything in this port that **cannot be verified without a board**, why it is
uncertain, and what would settle it. Written while porting, 2026-08-30.

**First hardware session: 2026-08-30.** The image boots on Board V1 and several
items below are now resolved — each is marked ✅ with what was observed. The
firmware also passes 89 host checks (`tests/`). Items still marked open have
genuinely not been exercised.

**Console:** there is no UART. Use `./rtt.sh [seconds]` for output, and the same
RTT channel carries a shell (`impulse buzz|motor|led|clock|chase|worn`) when
`CONFIG_IMPULSE_BRINGUP_SHELL=y`. `JLinkRTTLogger` **cannot find the control
block on this target** even when handed its exact address; the J-Link RTT telnet
server (port 19021, with `SetRTTSearchRanges`) does, which is what `rtt.sh` uses.

## Session 2026-09-11b — WiFi (chunk I) is up on BOTH boards

Build them with the new flags — the sysbuild symbols fail SILENTLY if omitted:

```sh
./build.sh watch  --cs --wifi
./build.sh anchor --cs --wifi
```

| | watch | anchor |
|---|---|---|
| associates (2.4 GHz, WPA2-PSK) | ✅ −58 dBm, ch1, WiFi 4 | ✅ −65 dBm, ch1, WiFi 4 |
| RAM | 284,424 B (54.4%) | 268,718 B (51.4%) |
| flash | 922,596 B (44.3%) | 843,468 B (40.5%) |
| BLE + WiFi together | ✅ 224 CS procedures with the link `COMPLETED` | ✅ iBeacon + RAS advertising while associated |

**RAM was 99.46% before tuning and is now 54%** — see `agent-notes.md`, 2026-09-11.
Dropping 5 GHz saves **nothing** (byte-identical build); keep `NRF_WIFI_2G_BAND`
for propagation, not for space.

**Still to do for WATCH_REMOVED (§5.5.1):** credentials are accepted over GATT
and dropped, there is no UDP sender, no anchor IP table, and no receiver on the
anchor. And the TRIGGER is worn detection, which remains unverified because the
IR window is behind the enclosure — the transport can be finished and still
never fire.

**Not yet exercised:** DHCP/IP (the `net` shell is not enabled), reconnection
after AP loss, and behaviour when WiFi and CS are both active for hours.

---

## Session 2026-09-11 — the CS link is gated on enforcement

**Root cause of "the anchor keeps dying":** the watch held the anchor's BLE link
open permanently, and a connectable advertiser stops advertising once connected.
The anchor was healthy throughout. Full reasoning in `agent-notes.md`, 2026-09-11.

**Fixed:** the CS engine now parks (disconnected, and NOT scanning) until a
commitment needs a distance, and releases the anchor within ~5 s of the window
ending. Verified on hardware, all three phases:

| watch state | anchor on air? | ranging |
|---|---|---|
| dormant | ✅ advertising, −73 dBm | off |
| enforcement | held by the watch (correct) | ✅ 172 estimates in 60 s |
| window ended | ✅ advertising again, −70 dBm | off |

**Accepted consequence:** at window start the burst buffer is empty while the
link is re-established and ~10 samples accumulate, so the first measurement
ABSTAINS. For `stayNear` that fails CLOSED — a brief alarm at window start —
which is the safe direction (§4.5). Do not "fix" it by reporting compliance
while the buffer fills.

**Anchor power:** the anchor reads `batt=0%` (AIN0 38–53 mV, against ~2070 mV on
the watch), i.e. **no usable cell is fitted**. It runs on external power and
resets when that glitches — one reset observed with only 372 s of uptime. §7
requires ≥90 min holdover; that is not currently met on this board.

**Still open:** the watch ranges whatever matches the iBeacon manufacturer
prefix, not the anchor `event.anchorId` names. Two anchors in a house = the
wrong one gets held and measured.

---

## Session 2026-09-10 — watch image with CS compiled in

Everything below was run against `build.sh watch` + `cs.conf` (`CONFIG_IMPULSE_CS_BACKEND=y`,
`CONFIG_BT_MAX_CONN=2`) on Board V1. **Item numbers refer to the sections further down.**

| Item | Result |
|---|---|
| 6 buzzer/motor | ✅ **Buzzer proven incapable of driving.** P3 `DIR` bit 7 never set during live enforcement; `PIN_CNF[7]=0` vs `0x2` on every unused pin. Motor pulses correctly and was seen pulsing by a human |
| 7 wake button | ✅ **All three classes fire**: SHORT, LONG (~2 s), VERY_LONG (~6 s). **Defect found and fixed:** one hold delivered VERY_LONG *twice*, 1.45 s apart — once while held, once on release |
| 8 storage | ✅ Schedule survives a reset (`save` -> reset -> 1 event). **But see the clock, below** |
| 9 enforcement timing | ✅ **Escalation measured to its floor.** Gaps 26.1 → 24.3 → 22.1 → 20.0 → 17.9 → 16.1 → 14.0 s, then flat at ~5.0 s for 26 cycles = `floor_interval_ms = 5000`. Bursts constant ~2 s |
| fail-safe | ✅ `stayNear` fails CLOSED (motor fires), `getAway` fails OPEN (silence). Criterion-dependent asymmetry confirmed on hardware |
| 4 LED ring | ✅ Decodes; **and the chain runs COUNTER-CLOCKWISE.** 09:30 rendered as 3:30 — a mirror about the 12–6 axis, not a rotation (a rotation would have shown 3:00). Fixed in `ring_pos()` |
| ring + motor together | ✅ **Driven simultaneously with a human watching** — clears `BLK-03` / `launch-plan.md` §10's claims fence |
| CS item 1 (engine starts) | ✅ after a fix — it was calling `bt_enable()` twice and aborting with `-120` |
| anchor reflector | 🟡 Anchor GATT intact alongside RAS: anchor service, `0x185B` with all six characteristics, `calib-v2` still absent, strap toggle answers `0x01`. **Ranging itself untested** |
| CS item 4 (concurrency) | 🟡 **Peripheral + central scan: PASSES.** `phone_sim` reads the full GATT surface, MTU 498, while the CS engine holds a scan. Not yet tested with an actual ranging session in progress — needs a reflector |
| 2 charger detect | ❌ **The pin never asserts.** Tested 2026-09-10 with a real USB-C plug event: battery rose 94% → 97%, so the TP4057 *was* charging, while `charger_detect` stayed `raw_pin=1 charging=0` throughout — the pull-up level, identical to unplugged. Not a polarity error; the line simply does not move. **Suspected cause: insufficient solder paste on the BM20C pad** (this board already had four unwetted module pads on 2026-08-29), so the pin is probably not connected to `CHRG` at all. NOT yet confirmed against the KiCad netlist, and P1.03 is still `[from schematic]`, never driven. **Decision (user, 2026-09-10): do not chase it.** Infer charging from the battery-voltage derivative instead — which is what v2 §6.3 did before Board V1 added this input. That inference is **not implemented yet.** Plugging in does NOT reset the board (uptime ran continuously across the event) |
| 1 worn detect | 🟡 **Still an estimate.** The IR window is covered by the enclosure, so no wrist reading was possible. Threshold raised 150 → 200 mV with a release at 140 mV (hysteresis added; there was none) |

### Resolved defects (see `agent-notes.md`, 2026-09-10)

1. `sys_reboot()` on every BLE disconnect — **any** disconnect rebooted the watch.
2. CS engine adopted the phone's connection as its ranging peer.
3. Vibration motor latched on forever on any non-"window ended" exit.
4. Bring-up shell silently disabled by `LOG_MODE_IMMEDIATE`.
5. `flash.sh` could not write images >256 KB (needs `r` before `halt`).
6. VERY_LONG button event delivered twice per hold.
7. CS restart loop spun on `-EALREADY` from `bt_scan_start()` — 47 times in 5 minutes.

### ✅ CLOSED the same evening — the wall clock now survives a reboot

**The finding, for the record.** `g_utc_base` in `src/main.c` was a RAM-only static
initialised to 0, so after any reset the watch believed it was 1970, matched no window, and
**enforced nothing** until the app reconnected and pushed the time — silently. Combined with
the `sys_reboot()` defect below it was a complete remote bypass: connect, disconnect,
enforcement stops.

**The decision that was taken.** The clock is persisted. `impulse/wall` is written every 60 s
and immediately on an app time-sync, and restored at boot behind a sanity floor of
2025-01-01. It is treated as a **lower bound, never as authority** — it lags by up to 60 s
plus off-time, so a commitment can start late but never early, and an app sync always
supersedes it. It is deliberately kept in a **separate settings key from the elapsed base**,
because §9.2 requires integrity timers on a monotonic basis precisely so that writing the
wall clock cannot accelerate a quarantine. Verified on hardware: `restored_from_flash=1`
after a reset, with the enforcement window correctly re-entered.

### ⚠️ OPEN, and now the most serious thing here

**Nothing has ever ranged against the reflector.** The reflector boots, registers the Ranging
Service and advertises `0x185B` over the air, and the initiator runs inside the watch image —
but no CS procedure has ever completed between them, because that needs two boards and only
one works. Every distance number this product depends on is still measured Board V1 → **DK**,
and the `+0.97 m` calibration constant is specific to that pair. Until a procedure completes
board-to-board, the product loop is open at its most important joint.

---

**Marker convention in the source:**

| Marker | Means |
|---|---|
| `TODO(hw)` | Needs a board to confirm. Compiles, may be wrong. |
| `TODO(api)` | Unsure the SDK call/binding is the right one. |
| `TODO(chunk X)` | Deliberately unimplemented; belongs to a later chunk (spec §10). |
| `[verified 2026-08-29]` | Exercised on Board V1 during bring-up. Trust these. |
| `[from schematic]` | Transcribed only. Never driven. |

```sh
grep -rn "TODO(hw)\|TODO(api)" src/ boards/
```

---

## P0 — will silently do the wrong thing

### 1. ADC channels — ✅ RESOLVED, mapping found empirically
**AIN0 is the battery divider, AIN1 is the IR receiver, and this part has only
EIGHT inputs (AIN0..AIN7), not the fourteen the dt-bindings header defines.**

Found by sweeping every input at boot while toggling the IR emitter, rather
than from a datasheet — the mapping is in none of our documents:

| ch | emitter off | emitter on | delta | verdict |
|---|---|---|---|---|
| AIN0 | 2069 mV | 2068 | −1 | **battery** — ×2 = 4138 mV, a charged cell |
| AIN1 | 137 mV | 73 | **−64** | **IR receiver** — the only channel that moves |
| AIN3 | 3309 | 3306 | −3 | sitting at the rail |

Two more findings on the way: `ADC_REF_VDD_1_4` is **not supported** on this
part (the driver rejects it), and the IR swing is **negative** — the ITR8307's
phototransistor pulls the node down under illumination, so more reflection
means a lower reading. `impulse_worn_sample()` returns dark-minus-lit so that
"more reflection" is a larger positive number.

Battery now reports over BLE (`batt=93%`). **Worn detection still needs its
threshold set from a real wrist**: on a desk with nothing in front of the
sensor the difference was 64 mV, which is emitter-to-detector crosstalk, and
`CONFIG_IMPULSE_WORN_THRESHOLD_MV` is a guess at 150.

### 1b. (original text, kept for the reasoning)
`src/hal/` has **no** battery-sense or IR-worn driver, because the AIN↔pin
mapping for the nRF54LM20A could not be established from anything on this
machine. `BM20C.pdf` lists analog functions only for the **BM15** module; the
BM20 columns show plain GPIO. Zephyr's `NRF_SAADC_AIN0..13` are silicon-fixed
but the pin they land on is not in any local document.

- **Blocks:** battery sense (`/BAT_DIV`, P1.00) and IR worn detection
  (`/IR_REC`, P1.29) — i.e. all of §5.2 worn detection and §6.3 charging.
- **Settle it with:** the nRF54LM20A product specification's GPIO table, or an
  empirical sweep — drive a known voltage onto P1.00 and read each AIN channel
  until one tracks it.
- **Do not guess.** A wrong channel reads a *different pin* and returns
  plausible numbers, which is the worst possible failure mode for a worn
  sensor: it would report the watch as worn when it is on a nightstand.

### 2. Charger-detect polarity is assumed
`charger_detect` (P1.03, pad C5) is declared `GPIO_ACTIVE_LOW` with a pull-up.
The TP4057's `CHRG` output is open-drain and pulls low while charging, so
active-low is the *likely* reading — but it is an assumption.

- **Test:** plug and unplug USB-C, watch the pin.
- **Consequence if wrong:** inverted charging state feeds §6.3's
  "is it on the charger" gate and the worn-detection hint, so the watch would
  believe it is charging exactly when it is not.

### 3. LFXO — ✅ RESOLVED (build config), drift measured separately
**The legacy `CLOCK.LFCLK` registers read all zero on this part and that is NOT
a fault.** `SRC=0`, `RUN=0`, `STAT=0` at `0x5010E440` looks exactly like the
silent RC fallback §3.2 warns about, and it is not: on nRF54L the GRTC manages
its own low-frequency source via `CONFIG_NRF_GRTC_TIMER_CLOCK_MANAGEMENT`
(`nrf_grtc_timer.c` calls `nrfx_grtc_clock_source_set(NRF_GRTC_CLKSEL_LFCLK)`)
rather than through the block older nRF parts used.

The authoritative signal is **`CONFIG_NRF_GRTC_TIMER_SOURCE_LFXO=y`**, which
this build resolves to. The boot banner now reports it. Anyone re-checking this
by reading `CLOCK.LFCLK` will conclude the crystal is unused and be wrong.

### 3b. (original text, kept for the reasoning)
The devicetree selects the on-module 32.768 kHz crystal. Spec §3.2 warns that
Zephyr falls back to the internal RC when nothing is specified and does so
**silently** — crystal unused, no error anywhere.

- **Test:** read the LFCLK source register at runtime and log it in the boot
  banner. Chunk D's "done when" requires exactly this.
- **Consequence if wrong:** RC drift over an 8 h sleep, and the Sunrise Lock
  silent-skip failure the spec says is not a live risk *because* of the LFXO.

---

## P1 — drivers written but never run

### 4. LED ring under Zephyr's `ws2812_spi` — 🟡 ELECTRICALLY CONFIRMED, visual check outstanding
**`impulse ledpin` reports 32% duty on P0.02 (3030 highs / 9447 samples)** while the
strip is driven white, which is right for 288 bits at 75% high followed by a
300 µs low reset. So SPIM30 genuinely puts a WS2812 waveform on the pad — this
is the check that PWM failed on 2026-08-29 while reporting perfect DMA.
**Still unconfirmed:** whether the LEDs decode it, and which pixel is index 0.
Run `impulse chase 3` and watch.

The bit timing is derived from measurements, not from the datasheet, and the
datasheet values are known to be **wrong for this chain**:

| | value | source |
|---|---|---|
| SPI clock | 8 MHz (16 MHz ÷ 2) | only achievable prescaler near the target |
| `spi-zero-frame` | `0xC0` → 2 bits → 0.25 µs | measured good |
| `spi-one-frame` | `0xFC` → 6 bits → 0.75 µs | **measured good; 0.5 µs decodes as a ZERO** |
| bit period | 1.0 µs (vs 1.25 µs nominal) | consequence of the 8 MHz clock |

- **Test:** `impulse_led_ring_set_all()` with a single lit pixel, then the
  clock face. Read the failure modes below rather than guessing.
- **Diagnostics (both invert the obvious reading):**
  - **Solid white, unchanging** → every bit read as a `1`; high time far too long.
  - **Uniformly dark and stable** → valid frames decoded as all-zero; `T1H`
    too short. These parts **hold** their last state on invalid signalling, so
    darkness means the chain *is* decoding, not that nothing arrived.
  - **Random flicker** → marginal signalling.
- **Open:** the 1.0 µs bit period is shorter than nominal. It worked bit-banged
  at 1.25 µs; whether the chain tolerates 1.0 µs is **untested**. If the ring
  misbehaves, try `spi-max-frequency = <4000000>` with `0xF0`/`0x80`
  (0.5 µs/0.25 µs at 250 ns per bit) as the first alternative.
- **Also unverified:** which physical LED is index 0. `impulse_led_ring_show_clock()`
  assumes pixel 0 is the 12-o'clock position; add an offset constant there
  rather than rotating the caller.

### 5. IMU (LIS3DHTR) over SPI — ✅ ENUMERATES
The driver initialises on every boot: `<inf> lis2dh: fs=2, odr=0x4 lp_en=0x0
scale=9576`. That means the part answered over SPI, so the shared P1 bus and
the **cross-domain CS on P0.00** both work. Still untested: sample data,
WHO_AM_I against a LIS3DHTR specifically, the INT1 wake path, and any
arbitration with the WM02C QSPI.

- `compatible = "st,lis2dh"` is Zephyr's family driver; the fitted part is a
  **LIS3DHTR**. Confirm `WHO_AM_I` matches before trusting any sample.
- **CS is cross-domain:** the bus is on P1 (`spi21`) while CS is `P0.00`.
  `cs-gpios` is a software GPIO toggle so this is legal, but it is untested and
  the timing margin is unknown.
- **The bus is shared with the WM02C's QSPI.** No arbitration exists in this
  code. Reading the IMU while WiFi is active is currently undefined behaviour —
  a chunk-I problem, flagged here so it is not discovered as a mystery.

### 6. Buzzer and motor — ✅ DRIVEN BY THE ENFORCEMENT PATH
Sampling P3 during a live `normal_both` window shows a ~2 s burst with
`motor=1 buzzer=1`, then the wait step — the profile table driving real pins.
**The buzzer needed `NRF_GPIO_DRIVE_H0H1`:** at standard drive the pad sags so
far into the buzzer's ~30 mA load that its own input buffer read it back LOW
while being driven high. Fixed in the devicetree.

Lowest risk on the board — both were driven from bare metal on 2026-08-29 and
the buzzer is confirmed **active** (a DC level sounds it, no tone generation
needed). What is unverified is only the Zephyr GPIO path.
- **Test:** run an enforcement profile and confirm the 2 s/30 s cadence and the
  2 s-per-cycle escalation down to the floor.

### 7. Wake button long-press seam
`impulse_buttons_init()` dispatches short/long/very-long. **Long and very-long
deliberately do nothing** — the hard override's mechanism is undecided
(`Concepts/Safety.md`, chunk J), and spec §6.2 says to leave the seam rather
than implement the v2 two-button hold, which this board cannot do.
- **Test:** confirm all three classify correctly, and that a pocket press does
  not fire the very-long path.

---

## P2 — logic that host tests cover but hardware may still break

### 8. Storage round-trip — ✅ WORKS, after two real defects
Saving now returns 0 and a schedule survives a reset (`schedule: 1 event(s),
tz offset -300 min` on the next boot). Two bugs had to be fixed first; both are
described in agent-notes, 2026-08-30. The elapsed base is now also written
every 10 minutes, so a power cycle rewinds integrity timers by at most that.

`impulse_storage_*` writes whole fixed-size structs through the settings
subsystem. Untested against real NVS.
- **Test:** write a schedule, reboot, read it back; then corrupt a record and
  confirm the size-mismatch guard rejects it rather than reinterpreting it.
- **The elapsed base is the sharp edge.** §9.2 requires integrity timers on a
  **monotonic elapsed** basis spanning reboots so that writing the clock cannot
  accelerate a quarantined loosening. `impulse_storage_save_elapsed_base()`
  exists and is loaded at boot, but **nothing saves it periodically yet**. Until
  that is wired, every reboot rewinds integrity timers toward zero.
  **Test explicitly:** quarantine a loosening, reboot, and confirm the 24 h does
  not restart.

### 9. Enforcement timing over a long window
Host tests drive the profile state machine with synthetic `dt_ms`. On hardware
the loop runs at 100 ms and will eventually be preempted by power management.
- **Test:** a full window with a non-compliant condition; confirm the escalation
  reaches its floor and that the condition going met stops output *immediately*.

---

## P3 — not implemented, listed so the gaps are visible

| Area | Chunk | State |
|---|---|---|
| BLE / GATT surface | E | ✅ **Working on hardware.** `phone_sim` scans, connects, reads status, sets time/settings, pushes schedules and reads the pending queue — unmodified, which is v3 §10's stated "done when". |
| Channel sounding | G | **Real backend exists** (`CONFIG_IMPULSE_CS_BACKEND`), builds at 347 KB / 27% RAM. Proven working as a standalone rig; **never run inside the product firmware**. Targets any RAS advertiser, not `event.anchorId`. |
| WiFi via WM02C | I | Power pins declared; no transport, no coex, no credentials. |
| Power management / `DORMANT_SLEEP` | D | Loop uses `k_msleep`. No PM, no wake sources. |
| Hard override | J | Button seam only. Gated on a design decision. |
| Anchor role behaviour | — | Kconfig selects it; the two images are currently **identical apart from a log string**. |
| Worn detection (§5.2) | B | Blocked on item 1. |
| Phone docking (§5) | — | `phoneAway` fusion is written but nothing supplies dock status. |

**The stub backend fails deliberately.** `impulse_cs_backend()->measure()`
always returns `IMPULSE_CS_FAIL_PROCEDURE`, which drives the §4.5 abstention
path. With no backend a `stayNear` commitment therefore fails **closed** and a
`getAway` fails **open** — the criterion-dependent fail-safe, working as
specified. A build that silently reported compliance would be far worse.

---

## Bring-up order

Use `.bringup/bringup-test.sh --continuity` first to confirm the board's pads
are still good, then:

1. Flash, confirm the RTT boot banner (there is **no UART** — spec §1.4).
2. Confirm LFXO (item 3).
3. LED ring (item 4) — the most likely thing to look broken while being fine.
4. Buzzer + motor via an enforcement profile (item 6).
5. Wake button dispatch (item 7).
6. Storage round-trip and the elapsed base (item 8).
7. Resolve the ADC mapping (item 1), then worn detection and battery.

Items 1–3 are the ones that fail *quietly*. Do them before trusting anything
built on top.


---

## Channel Sounding — what must be tested once the ground-truth sweep lands

The rig (`../nrf_cs_test`) already proved CS works on this silicon. These are
the tests specific to the **product firmware's** use of it.

1. **Does the engine start and range from inside the product image?**
   `./build.sh watch -- -DEXTRA_CONF_FILE="watch.conf;cs.conf"`, flash Board V1,
   put a reflector on the DK, and watch RTT for `CS backend: nrf-cs-ras`
   followed by burst results. Never done — the standalone rig is not the same
   thing as the engine running beside the enforcement loop.
2. **Does a real burst drive a real verdict?** `impulse demo 1 4` with a live
   reflector should flip `cond_met` 0→1 as the reflector comes within
   `NEAR_ENTER_CM`, and drive the motor/buzzer when it leaves. That is the
   whole product in one test.
3. **Anchor targeting.** The backend currently ranges against whatever
   advertises the RAS service. With two anchors in a house it will measure the
   wrong one — a correctness bug, not an optimisation.
4. **Concurrency.** CS holds a central connection while the app needs the watch
   to be a peripheral. Two simultaneous roles is exactly what broke the ESP32
   build (v2 §10.1, `ble_hs_timer_exp` panic), and Zephyr's host has its own
   version of that hazard.
5. **Calibrate `IMPULSE_CS_*_SLOPE_Q8`** in `cs_fuse.h` once ground truth
   exists. They currently encode how the estimators relate *to each other*,
   measured across ~10k samples — good enough for the quality and relay checks,
   useless as a distance calibration.
6. **Energy per procedure** at ~9 Hz free-running. The engine currently ranges
   continuously once connected, which is certainly wrong for an 8 h budget.


---

## Chunk E — verified on hardware 2026-08-30

`phone_sim` drives the watch **unmodified**, which is exactly the acceptance
criterion v3 §10 sets for this chunk.

```
watch  E1:06:CD:6C:4C:41  -67 dBm name='Impulse Watch'
dormant   bt=1 wifi=0 worn=0 batt=n/a  event=- cond=met  crc=5466f0fd
[watch] time push -> 0x01     [watch] settings push -> 0x01
[sched] END verdict 0x03: partial-quarantine (loosening held 24h)
  pending: delete d0d1d2d3  applies in 23h59m
```

**Commitment integrity ran over a real wire for the first time.** The push
deleted an existing event; the watch classified that as a loosening, applied
the rest immediately, and quarantined the deletion for 24 h. `watch clear`
answers `0x03` the same way — the behaviour CLAUDE.md calls out as the gate
working rather than failing. The pending countdown ticks on the elapsed-time
basis (24h00m → 23h59m across two reads).

### Five real defects this found, none visible from a clean build

1. **Stack overflow on the system workqueue.** `impulse_app_apply_schedule`
   held a `struct impulse_schedule` (~14 kB) on a ~4 kB workqueue stack, so
   sysworkq died the instant a real schedule arrived. Now static, which is safe
   because transfer is single-threaded.
2. **`activity_state` used the enum, not the wire encoding.** The wire is
   `0=dormant, 1=enforcement, 2=dormant_sleep`; the enum is a different order,
   so an idle watch reported itself as *enforcing*.
3. **`schedule_crc` was not persisted.** Per-event storage saved events but not
   the CRC, so after a reboot a synced watch reported "no schedule" and the app
   would re-push (MOBILE_APP_SPEC §8.16 exists to avoid exactly that).
4. **Advertising never restarted after disconnect.** `bt_le_adv_start()` was
   called from the disconnect callback, where it fails, and the return was
   ignored — the watch went invisible after the first connection with nothing
   in the log.
5. **Notifying from inside an ATT write callback** returns "Unlikely Error"
   (0x0E) to the client. All acknowledgements are now queued to a work item.

Four of those five are the same underlying rule: **do no real work, and send no
notification, on a Bluetooth callback context.** v3 §9 states it for storage;
it applies just as much to advertising and to notifications.

### Two tooling fixes that made the above findable

- **`CONFIG_LOG_BACKEND_RTT` was auto-selected**, and changing the log mode
  silently dropped it. `RTT_CONSOLE` stayed up so the boot banner still
  appeared while every `LOG_INF` vanished — indistinguishable from a crash.
  Pinned explicitly.
- **The RTT up-buffer was 1024 bytes and RTT drops rather than blocks**, so the
  Bluetooth init burst punched a hole in the boot log. Now 4096. The stack
  overflow above was only readable because logging is in immediate mode.

### Still open in chunk E

- **Concurrency with CS is UNTESTED.** `CONFIG_BT_MAX_CONN` is now 2 and the
  combined image builds (415 KB / 30% RAM), but peripheral+central has never
  been exercised together. This is the case that broke the ESP32 build.
- WiFi credentials, anchor IP table and LED config are accepted at the ATT
  layer and dropped; Seen Anchors returns an honest empty list.
- **BlueZ sometimes needs a re-`scan` between operations** after the watch
  re-advertises. Firmware-side restart is confirmed in the log; this looks like
  a host cache behaviour, not a device fault.


---

## Anchor role — verified on hardware 2026-08-30

The anchor image builds (241 KB / 21% RAM) and `phone_sim` drives it:

```
anchor E1:06:CD:6C:4C:41  uuid=49b4b0b2-4db0-4cae-9061-d60c1465da00
wifi:   state=0x00 (never provisioned)   dock: undocked   toggle: closed
calib-v2 char present: False
settings -> 0x01 (max_beep=5 min, tz=+0 min)
anchor schedule END -> 0x01
toggle open  -> 0x02      # refused: an active window involves this anchor
toggle close -> 0x01      # close is always accepted
```

- **iBeacon advertising works** and `phone_sim` recognises the device as an
  anchor, parsing its UUID out of the payload. Major is `0x4A0F`, the namespace
  fingerprint the watch filters on; Minor is `0x0000` because v3 §0.3 deletes
  the beacon-schedule slot tagging that used to encode it.
- **Identity is derived from FICR DEVICEID** and shaped into a valid UUIDv4, so
  it is stable per board without needing storage to have loaded.
- **The strap lock refuses to open during a commitment** (§4.9) while close is
  always accepted — the security-relevant half of that characteristic.
- **`calib-v2` is absent**, confirming the §4.8 deletion on the anchor side too.
- The anchor does **not** run the §9 integrity gate on its schedule, by design:
  the watch is the root of trust, and an anchor that quarantined its own
  schedule would just be out of sync with the device that decides.

### Anchor gaps

| Area | State |
|---|---|
| CS reflector | 🟡 **WRITTEN AND RUNNING, 2026-09-10** (`src/proximity/cs_reflector_nrf.c`, built with `cs_reflector.conf`). The anchor boots with `CS backend: nrf-cs-reflector`, registers the Ranging Service, and advertises `0x185B` in its SCAN RESPONSE — confirmed over the air. **Not yet proven to actually reflect**: no initiator has completed a procedure against it. |
| Dock status (§4.11) | Reports "no registered phone". Needs link-RSSI polling with `DOCK_UNDOCK_CONFIRM_POLLS=3`. |
| WiFi / UDP `WATCH_REMOVED` | No hardware (WM02C unpopulated), so beeping cannot currently be triggered remotely. The beep state machine itself is written and covers all three profiles. |
| Servo | Driver written, refusal logic verified, **never moved a real servo** — `U1` is unpopulated on this board. |
