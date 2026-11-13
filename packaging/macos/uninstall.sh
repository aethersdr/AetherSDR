#!/bin/bash
set -euo pipefail

echo "Uninstalling AetherSDR..."

if pgrep -x AetherSDR >/dev/null 2>&1; then
    echo "Quit AetherSDR before uninstalling." >&2
    exit 1
fi

if [ -d "/Applications/AetherSDR.app" ]; then
    sudo rm -rf -- "/Applications/AetherSDR.app"
    echo "  Removed AetherSDR.app"
fi

driver_removed=false
if [ -d "/Library/Audio/Plug-Ins/HAL/AetherSDRDAX.driver" ]; then
    sudo rm -rf -- "/Library/Audio/Plug-Ins/HAL/AetherSDRDAX.driver"
    echo "  Removed DAX virtual audio driver"
    driver_removed=true
fi

# Older DAX packages inadvertently installed libASPL development files.
# Remove only paths actually listed in AetherSDR's package receipt, never
# a separately installed libASPL or an unlisted file in those directories.
if pkgutil --pkg-info com.aethersdr.dax >/dev/null 2>&1; then
    legacy_files=()
    legacy_count=0
    while IFS= read -r path; do
        case "$path" in
            usr/local/include/aspl/*|usr/local/lib/cmake/libASPL/*|usr/local/lib/libASPL.a)
                if [[ "$path" != *".."* ]] && [ -f "/$path" ]; then
                    legacy_files+=("/$path")
                    legacy_count=$((legacy_count + 1))
                fi
                ;;
        esac
    done < <(pkgutil --files com.aethersdr.dax)
    if [ "$legacy_count" -gt 0 ]; then
        sudo rm -f -- "${legacy_files[@]}"
        sudo rmdir /usr/local/include/aspl /usr/local/lib/cmake/libASPL 2>/dev/null || true
        echo "  Removed development files installed by the old DAX package"
    fi
    sudo pkgutil --forget com.aethersdr.dax >/dev/null
    echo "  Forgot the DAX package receipt"
fi

if [ "$driver_removed" = true ]; then
    if sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod >/dev/null 2>&1; then
        echo "  Restarted CoreAudio"
    elif sudo killall coreaudiod >/dev/null 2>&1; then
        echo "  Restarted CoreAudio"
    else
        echo "  CoreAudio could not be restarted; reboot to unload the DAX devices." >&2
    fi
fi

printf "Also remove settings, caches, downloaded models and saved passwords and tokens? [y/N] "
read -r answer || answer=""
if [ "$answer" = "y" ] || [ "$answer" = "Y" ]; then
    rm -rf -- "$HOME/Library/Preferences/AetherSDR" \
              "$HOME/Library/Caches/AetherSDR" \
              "$HOME/Library/Application Support/AetherSDR" \
              "$HOME/.config/AetherSDR"
    for service in AetherSDR AetherSDR.aetherd; do
        while security delete-generic-password -s "$service" >/dev/null 2>&1; do
            :
        done
    done
    echo "  Removed user settings and caches"
fi

echo "Uninstall complete."
