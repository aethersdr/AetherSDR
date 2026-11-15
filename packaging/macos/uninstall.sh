#!/bin/bash
set -euo pipefail

echo "Uninstalling AetherSDR..."

# Remove app
if [ -d "/Applications/AetherSDR.app" ]; then
    rm -rf "/Applications/AetherSDR.app"
    echo "  Removed /Applications/AetherSDR.app"
fi

# Remove HAL plugin
if [ -d "/Library/Audio/Plug-Ins/HAL/AetherSDRDAX.driver" ]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/AetherSDRDAX.driver"
    echo "  Removed /Library/Audio/Plug-Ins/HAL/AetherSDRDAX.driver"
    # Restart CoreAudio
    sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod 2>/dev/null || true
    echo "  Restarted CoreAudio daemon"
fi

# Remove shared memory segments
for shm in /dev/shm/aethersdr-dax-*; do
    [ -e "$shm" ] && rm -f "$shm" && echo "  Removed $shm"
done

# Remove settings (optional)
read -p "Remove settings? [y/N] " answer
if [ "$answer" = "y" ] || [ "$answer" = "Y" ]; then
    rm -rf "$HOME/Library/Preferences/AetherSDR"
    echo "  Removed settings"
fi

echo "Uninstall complete."
