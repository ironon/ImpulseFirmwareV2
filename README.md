# Impulse firmware — nRF54LM20A / Zephyr

The nRF port of the Impulse watch and anchor firmware. One NCS application,
two roles, two images.

Impulse is a hardware **commitment device**: you set a rule for yourself in a
calm moment and the hardware holds you to it later. The defining property is
that **the enforcer is not the thing being bypassed** — the watch, not the
phone app, adjudicates schedule changes; tightening a commitment applies
immediately while loosening is quarantined ~24 h; and remove the watch and the
anchors take over.

> **Status: runs on Board V1, both roles, with the full Sunrise Lock loop
> demonstrated across two boards.** Channel sounding, BLE/GATT, WiFi and the
> UDP anchor-escalation path all work on hardware.
>
> **Read [`HARDWARE_TEST_PLAN.md`](HARDWARE_TEST_PLAN.md) before flashing
> anything.** It is the running list of what has actually been exercised
> versus what merely compiles, and it is kept honest deliberately.

## What works on hardware

| | |
|---|---|
| Boot, drivers, LED ring, motor, buzzer, wake button, IMU, ADC | ✅ |
| BLE/GATT surface, both roles — driven by `phone_sim` unmodified | ✅ |
| Commitment integrity (§9) — quarantines a real loosening over a real wire | ✅ |
| Channel sounding, Board V1 ↔ Board V1 | ✅ `ifft` sd 0.02–0.04 m |
| Enforcement escalation to its floor, criterion-dependent fail-safe | ✅ |
| WiFi (WM02C / nRF7002 over **SPI**) | ✅ both roles, 2.4 GHz |
| `WATCH_REMOVED` / `WATCH_WORN` over UDP → anchor alarm | ✅ end to end |
| Clock survives a reboot | ✅ |

## Not built

**WiFi firmware update / OTA**, the **hard override** (mechanism undecided),
**power management / `DORMANT_SLEEP`**, anchor **dock status**, and the servo
strap lock has never moved a real servo (`U1` is unpopulated).

Known-open correctness issue: the watch ranges **whatever matches the iBeacon
prefix**, not the anchor the event names. Harmless with one anchor, wrong with
two.

## Build

```sh
./build.sh watch  --cs --wifi     # watch: CS initiator + WiFi
./build.sh anchor --cs --wifi     # anchor: CS reflector + WiFi
./build.sh watch                  # plain
./build.sh watch --cs -p always   # pristine; west args pass through
```

Requires NCS **v3.4.0** at `~/ncs` (pinned in `west.yml`) and the Zephyr SDK.
Board target `impulse_v1/nrf54lm20a/cpuapp`, defined in `boards/impulse/impulse_v1/`.

**`--wifi` sets sysbuild symbols, and this is not cosmetic.** `CONFIG_WIFI_NRF70`
is a *sysbuild* symbol: setting it in an application `.conf` is discarded with
**no warning at all**, because `.config.sysbuild` is merged last. The result
builds cleanly, contains no WiFi driver, and every `wifi` command answers
*"No default interface found for type: STA"*. `build.sh` exists partly so that
trap cannot be stepped in again.

## Flash and console

```sh
./flash.sh watch        # SWD via an nRF54L15-DK's DEBUG OUT
./rtt.sh 30             # there is NO UART — console and shell are RTT only
./shell.sh "impulse status"
./cs_sample.sh 25 "2ft" # capture and summarise live CS estimates
```

## Host tests

```sh
cd tests && make        # 158 checks, ~1 s, no hardware
```

Builds the pure-logic sources with a host compiler. This is deliberate: the
three places the expensive bugs live — schedule recurrence, the §9 integrity
gate, and the verdict machine — are all pure logic, and they are worth being
able to test in a second without a board.

## Layout

| Path | What |
|---|---|
| `boards/impulse/impulse_v1/` | Board V1 definition, devicetree, pinctrl |
| `src/schedule/` | Data model, schedule wire format, day resolution |
| `src/integrity/` | Commitment integrity — classifier, diff gate, pending queue |
| `src/enforcement/` | Enforcement profiles and state machine |
| `src/proximity/` | Channel sounding: initiator, reflector, estimator fusion |
| `src/net/` | WiFi manager and the UDP anchor-escalation path |
| `src/ble/` | GATT services, watch and anchor |
| `src/hal/` | Board V1 drivers |
| `src/storage/` | Settings/NVS persistence |
| `tests/` | Host test suite |

## Things that are easy to get wrong here

- **Debug is RTT over SWD. There is no UART**, and no USB CDC — the USB-C
  connector carries power and CC only, D± deliberately unrouted.
  `CONFIG_SERIAL=n` is correct, not an oversight.
- **The LED ring must be driven by `SPIM30`, never PWM.** PWM20/21/22 cannot
  reach P0 — measured directly. The bit timing in the devicetree comes from
  bench measurement and is *deliberately outside* the SK6805 datasheet's
  nominal window; the datasheet value decodes as a zero on this chain. Don't
  "correct" it. The ring is also indexed **counter-clockwise**.
- **The WM02C is wired for SPI, not QSPI.** `QSPI-DATA2`/`DATA3` are physically
  unconnected on Board V1, so only `nordic,nrf7002-spi` can work. The bus is
  **shared with the IMU**, with separate chip selects.
- **Role is compile-time, never runtime.** A runtime-selectable role would let a
  watch be told it is an anchor, and an anchor does not enforce — which reduces
  the whole integrity chapter to one settings write.
- **Integrity timers use monotonic elapsed time, never wall clock**, so writing
  the clock cannot accelerate a quarantined loosening. Do not "fix" them to
  timestamps.
- **Abstention is not failure.** Below the quality floor the proximity layer
  returns no distance and does not guess. `stayNear` then fails **closed** and
  `getAway` fails **open** — that asymmetry is a threat-model property, because
  attenuation can fabricate "far" but never "near".
- **Ranging is gated on enforcement.** The watch connects to an anchor only
  while a commitment is being enforced, and releases it within ~5 s of the
  window ending. Holding the link permanently stops the anchor advertising —
  which for two days looked exactly like an anchor that kept dying.
- **Never notify or do real work from a Bluetooth callback.** Four separate
  defects here were the same rule.
- **Credentials never live in this tree.** SSID and passphrase arrive over GATT
  or are typed into the RTT shell at runtime.

## A note on the specs

Comments and docs reference sections such as "v3 §4.5" or "§9.3". Those are the
normative specs, which live in a **separate private vault repo** and are not
part of this repository. The code is written to be readable without them —
where a rule is load-bearing, the reasoning is in the comment next to it rather
than only in the spec.

## Bench-only hooks to remove before shipping

- `CONFIG_IMPULSE_BRINGUP_SHELL` — the whole `impulse …` RTT shell.
- `impulse wornset <0|1|auto>` — forces the worn state, because the IR window is
  behind the enclosure on the prototype. **Shell only, deliberately not
  reachable over BLE**: a forced worn state makes the watch believe a commitment
  is being honoured, which is exactly what the app must never assert.
- `impulse demo` — installs a synthetic schedule.
