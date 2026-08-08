#!/bin/bash
# assert-macos-deployment-floor.sh — Prove the staged .app can actually launch
# on the macOS it advertises.
#
# Run AFTER macdeployqt, because that is the first moment the bundle contains
# everything it will ship.
#
# dyld enforces LC_BUILD_VERSION minos on every Mach-O it loads, so the DMG's
# real minimum macOS is the MAXIMUM minos in the bundle — not
# CMAKE_OSX_DEPLOYMENT_TARGET, which only describes the app's own binary. That
# gap is not theoretical: Homebrew bottles are compiled for the OS they were
# built on, so the staged fftw/portaudio/hidapi carried the runner's floor (15.0
# on macos-15, 14.0 on macos-15-intel) into a bundle declaring 14.0 / 13.0, and
# an untargeted qtkeychain did the same. The app launched fine on the runner and
# on the maintainers' machines, and failed on exactly the older hardware the
# Intel artifact exists to serve (#4532). setup-macos-deps.sh and the
# MACOS_DEPLOYMENT_TARGET plumbing are the fix; this is the check that keeps it
# fixed, and the only one that sees dependencies nobody thought to look at.
#
# The staged Qt VERSION is asserted separately, in macos-dmg.yml — different
# claim, different failure mode.
#
# Usage: assert-macos-deployment-floor.sh <app-bundle> <deployment-target>

set -euo pipefail

APP="${1:-}"
FLOOR="${2:-}"

if [ -z "$APP" ] || [ -z "$FLOOR" ]; then
    echo "usage: $0 <app-bundle> <deployment-target>" >&2
    exit 2
fi
if [ ! -d "$APP" ]; then
    echo "ERROR: no bundle at $APP" >&2
    exit 1
fi

# Every minos in the file. Universal binaries carry one LC_BUILD_VERSION per
# slice, and they need not agree, so all of them are checked rather than the
# first. LC_VERSION_MIN_MACOSX is the pre-10.14 spelling — old prebuilt
# dependencies still use it.
minos_values() {
    otool -l "$1" 2>/dev/null | awk '
        /LC_BUILD_VERSION/     { bv=1; next }
        bv && /^ *minos/       { print $2; bv=0 }
        /LC_VERSION_MIN_MACOSX/{ vm=1; next }
        vm && /^ *version/     { print $2; vm=0 }
    '
}

# True when $1 is newer than $2.
newer_than() {
    [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]
}

echo "Checking every Mach-O in $APP against a macOS $FLOOR floor..."
violations=0
checked=0
worst="$FLOOR"

while IFS= read -r -d '' f; do
    file -b "$f" 2>/dev/null | grep -q 'Mach-O' || continue
    checked=$((checked + 1))
    while read -r minos; do
        [ -n "$minos" ] || continue
        if newer_than "$minos" "$worst"; then worst="$minos"; fi
        if newer_than "$minos" "$FLOOR"; then
            echo "  FAIL ${f#"$APP"/} needs macOS $minos"
            violations=$((violations + 1))
        fi
    done < <(minos_values "$f")
done < <(find "$APP" -type f -print0)

if [ "$checked" -eq 0 ]; then
    echo "ERROR: no Mach-O files found under $APP — the bundle is not staged." >&2
    exit 1
fi

if [ "$violations" -ne 0 ]; then
    echo >&2
    echo "ERROR: $violations binaries in the bundle need a newer macOS than the" >&2
    echo "       $FLOOR this DMG declares. dyld will refuse to load them and the" >&2
    echo "       app will crash on launch — the real floor is macOS $worst." >&2
    echo "       Either build the offending dependency at $FLOOR (see" >&2
    echo "       scripts/setup/setup-macos-deps.sh) or raise the deployment" >&2
    echo "       target to match what is actually being shipped." >&2
    exit 1
fi
echo "OK — $checked Mach-O files, all load on macOS $FLOOR."
