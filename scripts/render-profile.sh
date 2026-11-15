#!/bin/bash
# Profile AetherSDR render performance by watching log output
# Usage: ./scripts/render-profile.sh [duration_seconds]
#
# Requires AetherSDR built with render timing enabled (lcPerf log category).
# Captures paint event timing and computes statistics.

DURATION=${1:-30}

PID=$(pidof AetherSDR)
if [ -z "$PID" ]; then
    echo "AetherSDR is not running"
    exit 1
fi

LOGDIR="$HOME/.config/AetherSDR"
LATEST=$(ls -t "$LOGDIR"/aethersdr-*.log 2>/dev/null | head -1)

if [ -z "$LATEST" ]; then
    echo "No AetherSDR log file found in $LOGDIR"
    exit 1
fi

echo "Profiling AetherSDR render for ${DURATION}s..."
echo "Log: $LATEST"
echo ""

# Tail the log and capture paintEvent timing lines
TMPFILE=$(mktemp)
timeout "$DURATION" tail -f "$LATEST" 2>/dev/null | grep --line-buffered "paintEvent:" > "$TMPFILE" &
TAIL_PID=$!

# Show live output
tail -f "$TMPFILE" 2>/dev/null &
SHOW_PID=$!

sleep "$DURATION"
kill $TAIL_PID $SHOW_PID 2>/dev/null
wait $TAIL_PID $SHOW_PID 2>/dev/null

echo ""
echo "=== Render Statistics ==="

COUNT=$(wc -l < "$TMPFILE")
if [ "$COUNT" -eq 0 ]; then
    echo "No paintEvent timing data captured."
    echo "Enable the 'perf' log category in Help → Support to see render timing."
    rm "$TMPFILE"
    exit 0
fi

# Extract ms values and compute stats
awk '
/paintEvent:/ {
    match($0, /paintEvent: ([0-9]+)ms/, a)
    if (a[1] != "") {
        ms = a[1] + 0
        sum += ms
        count++
        if (ms > max) max = ms
        if (min == 0 || ms < min) min = ms
        if (ms > 16) slow++
    }
}
END {
    if (count > 0) {
        printf "Frames captured: %d\n", count
        printf "Average:         %.1f ms/frame\n", sum/count
        printf "Min:             %d ms\n", min
        printf "Max:             %d ms\n", max
        printf "Frames > 16ms:   %d (%.1f%%)\n", slow, 100*slow/count
        printf "Effective FPS:   %.1f\n", 1000/(sum/count)
    }
}' "$TMPFILE"

rm "$TMPFILE"
