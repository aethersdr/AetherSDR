#!/usr/bin/env python3
"""Render fixed boot units for review; this command never installs/enables them."""
import argparse
from pathlib import Path


def render(arch, managed_startup=False):
    if arch not in ('armv7', 'arm64'):
        raise ValueError('unknown radio architecture')
    store = '/user/aetherpd' if arch == 'armv7' else '/var/lib/smartsdr/aetherpd'
    session = store + '/pd-boot --session' if managed_startup else '/run/aetherpd/bin/pd-start-session'
    primary = store + '/pd-boot --primary' if managed_startup else '/run/aetherpd/bin/pd-primary-launch'
    return {
        'aetherpd-boot.service': f'''[Unit]
Description=AetherPD authenticated RAM payload
After=local-fs.target
Before=aetherpd-start.socket aetherpd-shim.service
[Service]
Type=oneshot
ExecStart={store}/pd-boot
RemainAfterExit=yes
TimeoutStartSec={90 if managed_startup else 30}
UMask=0077
''',
        'aetherpd-start.socket': f'''[Unit]
Description=AetherPD fixed local session entry
DefaultDependencies=no
Requires=aetherpd-boot.service
After=aetherpd-boot.service
Conflicts=shutdown.target
Before=shutdown.target
[Socket]
ListenStream=/run/aetherpd/start.sock
SocketMode=0600
Accept=yes
MaxConnections=2
ExecStartPost={store}/pd-boot --socket-owner
TimeoutSec=10
''',
        'aetherpd-start@.service': f'''[Unit]
Description=AetherPD owned local session
Requires=aetherpd-boot.service
After=aetherpd-boot.service
[Service]
ExecStart={session}
StandardInput=socket
StandardOutput=journal
StandardError=journal
TimeoutStopSec=12
KillMode=control-group
LimitNOFILE=1024
UMask=0077
''',
        'aetherpd-shim.service': f'''[Unit]
Description=AetherPD ordinary-user control service
Requires=aetherpd-boot.service aetherpd-start.socket
After=aetherpd-boot.service aetherpd-start.socket
[Service]
ExecStart={primary}
StandardOutput=journal
StandardError=journal
TimeoutStopSec=12
KillMode=control-group
UMask=0077
[Install]
WantedBy=multi-user.target
''',
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arch', choices=['armv7', 'arm64'], required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--managed-startup', action='store_true')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    for name, contents in render(a.arch, a.managed_startup).items():
        (a.output / name).write_text(contents)


if __name__ == '__main__':
    main()
