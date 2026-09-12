#!/usr/bin/env bash
# Build helper.
#
#   ./build.sh watch                   # plain watch
#   ./build.sh watch --cs              # + channel sounding (initiator)
#   ./build.sh watch --cs --wifi       # + WiFi (WM02C)
#   ./build.sh anchor --cs --wifi      # anchor: CS reflector + WiFi
#   ./build.sh watch --cs -p always    # pristine; west args pass through
#
# BOARD_ROOT has to be on the command line rather than in CMakeLists.txt:
# sysbuild resolves the board before the application's CMakeLists runs, so
# setting it there is too late and the board is "not found".
set -euo pipefail
ROLE="${1:-watch}"; shift || true
case "$ROLE" in
  watch|anchor) ;;
  *) echo "usage: $0 {watch|anchor} [--cs] [--wifi] [west args]"; exit 2 ;;
esac

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFS="$ROLE.conf"
OVERLAYS=""
SBFLAGS=()
SUFFIX=""

# CS is the initiator on the watch and the reflector on the anchor — the same
# flag, because "does this build do channel sounding" is the question a caller
# actually has.
WANT_CS=0; WANT_WIFI=0; WANT_COEX=0; WEST_ARGS=()
for a in "$@"; do
  case "$a" in
    --cs)   WANT_CS=1 ;;
    --wifi) WANT_WIFI=1 ;;
    --coex) WANT_COEX=1 ;;
    *)      WEST_ARGS+=("$a") ;;
  esac
done

if [ "$WANT_CS" = 1 ]; then
  if [ "$ROLE" = "anchor" ]; then CONFS="$CONFS;cs_reflector.conf"; else CONFS="$CONFS;cs.conf"; fi
  SUFFIX="${SUFFIX}cs"
fi

if [ "$WANT_WIFI" = 1 ]; then
  # wifi_lean.conf MUST come after wifi.conf — it overrides the stock nRF70
  # buffer sizes, and without it CS + WiFi is 99.5% RAM and does not fit.
  CONFS="$CONFS;wifi.conf;wifi_lean.conf"
  OVERLAYS="$HERE/wifi.overlay"
  # THESE TWO ARE NOT OPTIONAL AND FAIL SILENTLY IF OMITTED.
  # CONFIG_WIFI_NRF70 is a SYSBUILD symbol: setting it in an application conf
  # is discarded with no warning, because .config.sysbuild is merged last. The
  # result builds cleanly, contains no WiFi driver at all, and every `wifi`
  # command answers "No default interface found for type: STA".
  SBFLAGS+=(-DSB_CONFIG_WIFI_NRF70=y -DSB_CONFIG_WIFI_NRF70_SYSTEM_MODE=y)
  SUFFIX="${SUFFIX}wifi"
fi

if [ "$WANT_COEX" = 1 ]; then
  if [ "$WANT_WIFI" != 1 ]; then
    echo "--coex requires --wifi"; exit 2
  fi
  CONFS="$CONFS;coex.conf"
  OVERLAYS="$OVERLAYS;$HERE/coex.overlay"
  SUFFIX="${SUFFIX}coex"
fi

BUILD_DIR="$HERE/build-$ROLE${SUFFIX:+-$SUFFIX}"
source ~/ncs/env.sh
set -x
exec west build -b impulse_v1/nrf54lm20a/cpuapp -d "$BUILD_DIR" "${WEST_ARGS[@]}" "$HERE" \
     -- -DBOARD_ROOT="$HERE" -DEXTRA_CONF_FILE="$CONFS" \
        ${OVERLAYS:+-DEXTRA_DTC_OVERLAY_FILE="$OVERLAYS"} "${SBFLAGS[@]}"
