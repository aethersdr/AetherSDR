#!/usr/bin/env python3
"""Fixed FLEX-8000 installer units; rendering never installs or starts them."""


def render():
    common = '''[Service]
Type=oneshot
Restart=on-failure
RestartSec=5
RestartPreventExitStatus=2 3
TimeoutStartSec=240
TimeoutStopSec=15
KillMode=control-group
UMask=0077
'''
    prefix = '[Unit]\nAfter=local-fs.target smartsdr.service podman-restart.service\nStartLimitIntervalSec=60\nStartLimitBurst=3\n'
    path = '/var/lib/smartsdr/aetherpd/pd-boot'
    return {
        'aetherpd-install.service': prefix + 'Description=AetherPD approved installation worker\n' + common + f'ExecStart={path} --install-approved\n',
        'aetherpd-recover.service': prefix + '''Description=AetherPD interrupted installation recovery
Before=aetherpd-boot.service aetherpd-start.socket aetherpd-shim.service
''' + common + f'ExecStart={path} --recover-installation\n[Install]\nWantedBy=multi-user.target\n',
    }
