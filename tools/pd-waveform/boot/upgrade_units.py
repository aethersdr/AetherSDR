#!/usr/bin/env python3
"""Fixed temporary upgrade units; no commands are installed or executed here."""

def render(arch):
    if arch not in ('arm64', 'armv7'):
        raise ValueError('unknown radio architecture')
    parent = '/user' if arch == 'armv7' else '/var/lib/smartsdr'
    firmware = 'flex6k-firmware.service' if arch == 'armv7' else 'smartsdr.service'
    prefix = '[Unit]\nAfter=local-fs.target ' + firmware + '\n'
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
    if arch == 'arm64':
        common += ('CapabilityBoundingSet=CAP_CHOWN CAP_DAC_OVERRIDE CAP_FOWNER CAP_SYS_PTRACE\n'
                   'NoNewPrivileges=yes\n')
    worker = parent + '/aetherpd-upgrade/worker'
    return {
        'aetherpd-upgrade.service': prefix + 'Description=AetherPD approved upgrade worker\n' + common + f'ExecStart={worker}\n',
        'aetherpd-upgrade-recover.service': prefix + 'Description=AetherPD interrupted upgrade recovery\n' + common + f'ExecStart={worker} --recover\n[Install]\nWantedBy=multi-user.target\n',
    }
