#!/usr/bin/env python3
"""Build a bounded ordinary-user CPU regression diagnostic; never installs it."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
NAME = 'AetherPDCpuProbe'
LIMIT = 512 * 1024
CASES = ('recovery_collection', 'eq_phase_startup', 'warm_core', 'eq_worker', 'dispatch_deadline')
CFG = '''[header]
Name: AetherPDCpuProbe
Version: "0.1.0"
Minimum-SmartSDR-Version: 4.2.18.0
Author: AetherSDR
License: GPL-3.0
Executable: "probe.sh"

[setup]

[end]
'''
LAUNCHER = '''#!/bin/sh
set -eu
case "$0" in
  */*) probe_dir=${0%/*} ;;
  *) probe_dir=. ;;
esac
case "$(uname -m)" in
  aarch64|arm64) exec "$probe_dir/pd-cpu-probe-arm64" --serve ;;
  armv7l|armv7|arm) exec "$probe_dir/pd-cpu-probe-armv7" --serve ;;
  *) exit 2 ;;
esac
'''


def build(output, host=False):
    specification = importlib.util.spec_from_file_location('pd_waveform_build', HERE.parent / 'build.py')
    dsp = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(dsp)
    output.mkdir(parents=True, exist_ok=True)
    files = {NAME + '.cfg': (CFG.encode(), 0o644), 'probe.sh': (LAUNCHER.encode(), 0o755),
             'LICENSE': ((HERE.parents[2] / 'LICENSE').read_bytes(), 0o644),
             'LICENSES': (dsp.licenses().encode(), 0o644)}
    with tempfile.TemporaryDirectory(prefix='pd-cpu-probe-', dir=output) as temporary:
        stage = Path(temporary)
        sources = []
        for name in CASES:
            source = (HERE.parent / (name + '_test.c')).read_text()
            if source.count('int main(void)') != 1:
                raise ValueError('unexpected regression entry point')
            target = stage / (name + '_case.c')
            source = source.replace('waveform_program_main', 'pd_unused_waveform_' + name)
            target.write_text(source.replace('int main(void)', 'int pd_case_' + name + '(void)'))
            sources.append(target)
        sources.append(HERE.parent / 'warm_seed_curve.c')
        targets = [('armv7', dsp.ARMV7), ('arm64', dsp.ARM64)]
        if host:
            targets.append(('host', ['-target', 'x86_64-linux-musl']))
        for arch, target in targets:
            binary = stage / ('pd-cpu-probe-' + arch)
            flags = ('-UNDEBUG', *dsp.WDSP_INC, '-DPD_CPU_PROBE_HOST') if arch == 'host' else ('-UNDEBUG', *dsp.WDSP_INC)
            flags += ('-ffile-prefix-map=' + str(stage) + '=./cpu-probe-generated',)
            dsp.zig_cc(target, binary, stage / 'cache', entry_source=HERE / 'cpu_probe.c',
                       extra_cflags=flags, extra_sources=sources, embedded_local_iq=True)
            if arch == 'host':
                (output / binary.name).write_bytes(binary.read_bytes())
                (output / binary.name).chmod(0o755)
            else:
                files[binary.name] = (binary.read_bytes(), 0o755)
    target = output / (NAME + '.ssdr_waveform')
    pending = target.with_suffix('.tmp')
    with zipfile.ZipFile(pending, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, (data, mode) in sorted(files.items()):
            entry = zipfile.ZipInfo(NAME + '/' + name, (2026, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = (0o100000 | mode) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data, compresslevel=9)
    if pending.stat().st_size > LIMIT:
        pending.unlink()
        raise ValueError('CPU diagnostic package exceeds512KiB')
    pending.replace(target)
    return {'package': str(target), 'bytes': target.stat().st_size,
            'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
            'compiler': subprocess.check_output(['zig', 'version'], text=True).strip(),
            'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                      for name, (data, _) in files.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--host', action='store_true')
    args = parser.parse_args()
    print(json.dumps(build(args.output, args.host), indent=2))


if __name__ == '__main__':
    main()
