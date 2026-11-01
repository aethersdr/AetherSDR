#!/bin/sh
# Launches the AetherSDR radio probe built for this radio's CPU. GPL-3.0.
case "$0" in
  */*) dir=${0%/*} ;;
  *) dir=. ;;
esac
m=$(uname -m 2>/dev/null)
PROBE_UNAME_M=$m
# uname -srvm: -a would add the hostname, a Flex radio's serial number.
PROBE_UNAME_A=$(uname -srvm 2>/dev/null)
export PROBE_UNAME_M PROBE_UNAME_A
case "$m" in
  aarch64|arm64) exec "$dir/radio-probe-arm64" ;;
  # Any other 32-bit ARM tries the ARMv7 build: if the CPU can't run it, the
  # crash says so as clearly as refusing would.
  arm*) exec "$dir/radio-probe-armv7" ;;
  *) echo "radio-probe: no build for CPU '$m'" >&2; exit 1 ;;
esac
