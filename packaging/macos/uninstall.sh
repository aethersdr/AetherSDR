#!/bin/bash
set -euo pipefail

echo "Uninstalling MasterSDR..."

# Remove app
if [ -d "/Applications/MasterSDR.app" ]; then
    rm -rf "/Applications/MasterSDR.app"
    echo "  Removed /Applications/MasterSDR.app"
fi

# Remove HAL plugin
if [ -d "/Library/Audio/Plug-Ins/HAL/MasterSDRDAX.driver" ]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/MasterSDRDAX.driver"
    echo "  Removed /Library/Audio/Plug-Ins/HAL/MasterSDRDAX.driver"
    # Restart CoreAudio
    sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || true
    echo "  Restarted CoreAudio daemon"
fi

# Remove shared memory segments
for shm in /dev/shm/MasterSDR-dax-*; do
    [ -e "$shm" ] && rm -f "$shm" && echo "  Removed $shm"
done

# Remove settings (optional)
read -p "Remove settings? [y/N] " answer
if [ "$answer" = "y" ] || [ "$answer" = "Y" ]; then
    rm -rf "$HOME/Library/Preferences/MasterSDR"
    echo "  Removed settings"
fi

echo "Uninstall complete."
