#!/bin/sh
# Starts flex-tailnet-shim as a legacy (native) waveform on a FLEX-6000.
# Part of AetherSDR, GPL-3.0. FLEX-8000 and Aurora radios run the shim in
# their Docker container instead.
case "$0" in
  */*) dir=${0%/*} ;;
  *) dir=. ;;
esac
case "$(uname -m 2>/dev/null)" in
  armv7*) ;;
  *) echo "flex-tailnet-shim: this package is for FLEX-6000 radios (ARMv7); use the container shim on this radio" >&2; exit 1 ;;
esac
# The node's identity and settings persist beside the waveform (on a
# FLEX-6300 that's the /user partition); fall back to /var/tmp, which also
# survived a reboot there. Logs and caches stay in RAM.
state="$dir/state"
if ! mkdir -p "$state" 2>/dev/null || ! touch "$state/.w" 2>/dev/null; then
  state=/var/tmp/aether-tailnet
fi
rm -f "$state/.w" 2>/dev/null
SHIM_STATE_DIR=$state
SHIM_RUN_DIR=/tmp/aether-tailnet
SSDR_RADIO_ADDRESS=${SSDR_RADIO_ADDRESS:-127.0.0.1}
# No log uploads to Tailscale: they cost CPU this radio's single core needs.
TS_NO_LOGS_NO_SUPPORT=true
export SHIM_STATE_DIR SHIM_RUN_DIR SSDR_RADIO_ADDRESS TS_NO_LOGS_NO_SUPPORT
# Set at build time for diagnostics (build.py --log-url); empty otherwise.
SHIM_LOG_URL=""
[ -n "$SHIM_LOG_URL" ] && export SHIM_LOG_URL
# Below SmartSDR, so the radio wins when the one core is contended.
if command -v nice >/dev/null 2>&1; then
  exec nice -n 10 "$dir/flex-tailnet-shim-armv7"
fi
exec "$dir/flex-tailnet-shim-armv7"
