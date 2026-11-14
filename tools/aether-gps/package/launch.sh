#!/bin/sh
# Starts AetherGPS on a FLEX-6000, which shares the radio's GPS on the LAN:
# the gpsd protocol on TCP 2947 and NMEA 0183 on UDP 10110. FLEX-8000 and
# Aurora radios take the container image instead. Part of AetherSDR, GPL-3.0.
case "$0" in
  */*) dir=${0%/*} ;;
  *) dir=. ;;
esac
case "$(uname -m 2>/dev/null)" in
  armv7*) bin=$dir/aether-gps-armv7 ;;
  *) echo "aether-gps: this package is for FLEX-6000 radios (ARMv7), not $(uname -m)" >&2; exit 1 ;;
esac
if command -v nice >/dev/null 2>&1; then
  exec nice -n 19 "$bin"
fi
exec "$bin"
