#!/bin/sh
# Starts AetherWSPR, a receive-only WSPR receiver that serves its spots to
# AetherSDR on the LAN (TCP 48995). Part of AetherSDR, GPL-3.0-or-later.
case "$0" in
  */*) dir=${0%/*} ;;
  *) dir=. ;;
esac
case "$(uname -m 2>/dev/null)" in
  armv7*) bin=$dir/aether-wspr-armv7 ;;
  aarch64) bin=$dir/aether-wspr-arm64 ;;
  *) echo "aether-wspr: unsupported CPU $(uname -m)" >&2; exit 1 ;;
esac
exec "$bin"
