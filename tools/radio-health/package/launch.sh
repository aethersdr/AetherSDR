#!/bin/sh
# Starts AetherHealth, which reports the radio's free storage, memory and
# CPU to AetherSDR on the LAN (TCP 48994). Part of AetherSDR, GPL-3.0.
case "$0" in
  */*) dir=${0%/*} ;;
  *) dir=. ;;
esac
case "$(uname -m 2>/dev/null)" in
  armv7*) bin=$dir/aether-health-armv7 ;;
  aarch64) bin=$dir/aether-health-arm64 ;;
  *) echo "aether-health: unsupported CPU $(uname -m)" >&2; exit 1 ;;
esac
# Free space, most useful first: a FLEX-8000's container store, then the
# partition legacy waveforms share (a FLEX-6000's /user). Unreadable paths
# are skipped.
if command -v nice >/dev/null 2>&1; then
  exec nice -n 19 "$bin" /var/lib/smartsdr/containers "$dir"
fi
exec "$bin" /var/lib/smartsdr/containers "$dir"
