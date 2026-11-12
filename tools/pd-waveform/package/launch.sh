#!/bin/sh
# Starts AetherPD, a PureSignal-style predistortion waveform (mode "PD").
# Part of AetherSDR, GPL-3.0-or-later.
#
# Feedback: an external coupler on the transmit side's receive input (RX A
# for ANT1), with full duplex on. AetherPD makes its own receive-only slice
# there, 3.5 kHz below the carrier, and listens on DAX audio channel 4. Change
# the defaults here if your setup differs.
: "${AETHER_PD_FEEDBACK:=audio}"
: "${AETHER_PD_DAX:=4}"
: "${AETHER_PD_FB_ANT:=RX_A}"
: "${AETHER_PD_PEAK:=0.7}"
export AETHER_PD_FEEDBACK AETHER_PD_DAX AETHER_PD_FB_ANT AETHER_PD_PEAK
case "$0" in
  */*) dir=${0%/*} ;;
  *) dir=. ;;
esac
case "$(uname -m)" in
  aarch64|arm64) exec "$dir/aether-pd-arm64" ;;
  *) exec "$dir/aether-pd-armv7" ;;
esac
