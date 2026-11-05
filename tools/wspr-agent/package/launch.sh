#!/bin/sh
# Starts AetherWSPR, a receive-only WSPR receiver that serves its spots to
# AetherSDR on the LAN (TCP 48995). Part of AetherSDR, GPL-3.0-or-later.
case "$0" in
  */*) dir=${0%/*} ;;
  *) dir=. ;;
esac
exec "$dir/aether-wspr-armv7"
