#!/bin/sh
# Runs discovery_helper.py so that every program on this computer that
# lists FlexRadios (AetherSDR and SmartSDR for Mac, say) sees a radio on the
# tailnet, not just whichever one opened first. Press Ctrl+C to stop it.
#
# The radio's tailnet name defaults to flex-radio. Set TAILNET_SHIM to change
# it (export TAILNET_SHIM=my-radio, e.g. in ~/.zshrc), or pass --shim my-radio.
# Other arguments go to the helper.
#
# On macOS and Linux the helper broadcasts to 255.255.255.255, which also goes
# out on this computer's network: macOS delivers a loopback broadcast to no
# program at all. If the send fails with "No route to host" (macOS with two
# network interfaces up), pass --broadcast with that interface's broadcast
# address instead.

here=$(cd "$(dirname "$0")" && pwd)
shim=${TAILNET_SHIM:-flex-radio}

python=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 &&
        "$candidate" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then
        python=$candidate
        break
    fi
done
if [ -z "$python" ]; then
    echo "Python 3.8 or newer was not found." >&2
    exit 1
fi

exec "$python" "$here/discovery_helper.py" --shim "$shim" "$@"
