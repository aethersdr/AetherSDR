#!/usr/bin/env python3
"""Compare test-only dead-section removal without changing compiler inputs."""
import json
from pathlib import Path
import shutil
import subprocess
import time


def call(args):
    subprocess.run(args, check=True)


def capture(args):
    return subprocess.check_output(args, text=True)


flags = ['cmake', '-B', 'build', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
         '-DENABLE_NVIDIA_AFX=ON', '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG',
         '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG',
         '-DAETHER_GPU_SPECTRUM=ON', '-DCMAKE_C_COMPILER_LAUNCHER=ccache',
         '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
         '-DAETHER_CI_STRIP_TEST_DEBUG=ON']
report = {'source_parent': '8672486cfcf49dc500d98ca00a6726ae4c48d2b3',
          'compiler': capture(['c++', '--version']).splitlines()[0],
          'linker': capture(['ld', '--version']).splitlines()[0]}
try:
    for mode in ['strip', 'gc']:
        link = '-Wl,--compress-debug-sections=zlib-gabi'
        if mode == 'gc':
            link += ' -Wl,--gc-sections'
        link += ' -Wl,-Map,probe-link.map'
        call(flags + ['-DCMAKE_EXE_LINKER_FLAGS=' + link])
        commands = Path('build/compile_commands.json').read_text()
        Path('probe-commands-' + mode + '.json').write_text(commands)
        call(['ccache', '--zero-stats'])
        started = time.monotonic()
        call(['cmake', '--build', 'build', '--target', 'tci_rx_audio_test', '--parallel', '4'])
        report[mode] = {'build_seconds': round(time.monotonic()-started, 3),
                        'binary_bytes': Path('build/tci_rx_audio_test').stat().st_size}
        rows = Path('build/.ninja_log').read_text().splitlines()
        links = [r.split('\t') for r in rows if '\ttci_rx_audio_test\t' in r]
        report[mode]['link_milliseconds'] = int(links[-1][1])-int(links[-1][0])
        Path('probe-stats-' + mode + '.txt').write_text(capture(['ccache','--show-stats']))
        Path('probe-sections-' + mode + '.txt').write_text(
            capture(['readelf','--wide','--section-headers','build/tci_rx_audio_test']))
        names = capture(['nm','-S','--size-sort','build/tci_rx_audio_test']).splitlines()
        Path('probe-largest-symbols-' + mode + '.txt').write_text('\n'.join(names[-80:])+'\n')
        shutil.copyfile('build/probe-link.map', 'probe-map-' + mode + '.txt')
    report['compiler_commands_identical'] = Path('probe-commands-strip.json').read_bytes() == Path('probe-commands-gc.json').read_bytes()
finally:
    Path('probe-gc-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('GC_PROBE_REPORT '+json.dumps(report),flush=True)
