#!/bin/bash
# Monitor AetherSDR per-thread CPU usage in real-time
# Usage: ./scripts/cpu-monitor.sh [interval_seconds]
#
# Shows each thread's CPU%, sorted by usage. Updates every N seconds (default 2).

INTERVAL=${1:-2}

PID=$(pidof AetherSDR)
if [ -z "$PID" ]; then
    echo "AetherSDR is not running"
    exit 1
fi

echo "Monitoring AetherSDR (PID $PID) — per-thread CPU usage"
echo "Press Ctrl+C to stop"
echo ""

while true; do
    clear
    echo "=== AetherSDR Thread CPU Usage (PID $PID) — $(date +%H:%M:%S) ==="
    echo ""
    printf "%-8s %-25s %6s %6s %8s\n" "TID" "THREAD NAME" "%CPU" "%MEM" "TIME+"
    echo "------------------------------------------------------------"

    # ps with thread display, sorted by CPU descending
    ps -L -p "$PID" -o lwp=,comm=,pcpu=,pmem=,cputime= --sort=-pcpu 2>/dev/null | \
    while read lwp comm cpu mem time; do
        # Try to get Qt thread name from /proc
        tname="$comm"
        if [ -f "/proc/$PID/task/$lwp/comm" ]; then
            tname=$(cat "/proc/$PID/task/$lwp/comm" 2>/dev/null || echo "$comm")
        fi
        printf "%-8s %-25s %5s%% %5s%% %8s\n" "$lwp" "$tname" "$cpu" "$mem" "$time"
    done

    echo ""
    echo "Total threads: $(ls /proc/$PID/task/ 2>/dev/null | wc -l)"
    echo "Total CPU: $(ps -p $PID -o pcpu= 2>/dev/null | tr -d ' ')%"

    sleep "$INTERVAL"
done
