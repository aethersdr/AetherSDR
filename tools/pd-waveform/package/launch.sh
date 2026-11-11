#!/bin/sh
# Starts AetherPD, a PureSignal-style predistortion waveform (mode "PD").
# Part of AetherSDR, GPL-3.0-or-later.
#
# Feedback: put a panadapter on the receive input that the external coupler
# feeds (RX A for ANT1), centred on the transmit frequency, with DAX IQ
# channel 1. Change the defaults here if your setup differs.
: "${AETHER_PD_DAXIQ:=1}"
: "${AETHER_PD_DAXIQ_RATE:=48000}"
: "${AETHER_PD_PEAK:=0.7}"
export AETHER_PD_DAXIQ AETHER_PD_DAXIQ_RATE AETHER_PD_PEAK
case "$0" in
  */*) dir=${0%/*} ;;
  *) dir=. ;;
esac
case "$(uname -m)" in
  aarch64|arm64) exec "$dir/aether-pd-arm64" ;;
  *) exec "$dir/aether-pd-armv7" ;;
esac
