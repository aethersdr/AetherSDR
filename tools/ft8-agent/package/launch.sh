#!/bin/sh
# Starts AetherFT8, an FT8 receiver that decodes on the radio and serves its
# spots to AetherSDR on the LAN (TCP 48997). It does not transmit.
# Part of AetherSDR, GPL-3.0-or-later.
case "$0" in
  */*) dir=${0%/*} ;;
  *) dir=. ;;
esac
exec "$dir/aether-ft8-armv7"
