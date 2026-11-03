#!/usr/bin/env python3
"""Bounded, read-only-cache comparison of one real core compiler invocation."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

BASE = '106b528cc50476cf0ffbc3bf381ed335ef84e7ee'
ROOT = Path.cwd()
TARGET = 'CMakeFiles/aethercore.dir/src/core/AudioEngine.cpp.o'
FLAGS = ['cmake', '-B', 'build', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
         '-DENABLE_NVIDIA_AFX=ON', '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG',
         '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG',
         '-DAETHER_GPU_SPECTRUM=ON', '-DCMAKE_C_COMPILER_LAUNCHER=ccache',
         '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']

def call(args, **kw):
    subprocess.run(args, check=True, **kw)

def capture(args):
    return subprocess.check_output(args, text=True)

safe_git = ['git', '-c', f'safe.directory={ROOT}']
call(safe_git + ['fetch', '--depth=1', 'origin', BASE])
baseline = capture(safe_git + ['show', BASE + ':tests/tests.cmake'])
original = Path('tests/tests.cmake').read_text()
report = {'baseline': BASE, 'compiler': capture(['c++', '--version']).splitlines()[0],
          'ccache_version': capture(['ccache', '--version']),
          'ccache_config': capture(['ccache', '--show-config'])}
Path('probe-cache-before.txt').write_text(capture(['du', '-sh', '/ccache']))
try:
    for mode in ['pr', 'baseline', 'shell_locale']:
        Path('tests/tests.cmake').write_text(original if mode == 'pr' else baseline)
        call(FLAGS + ['-DAETHER_CI_STRIP_TEST_DEBUG=' + ('ON' if mode == 'pr' else 'OFF'),
                      '-DCMAKE_EXE_LINKER_FLAGS=' +
                      ('-Wl,--compress-debug-sections=zlib-gabi' if mode == 'pr' else '')])
        commands = Path('build/compile_commands.json').read_text()
        Path('probe-commands-' + mode + '.json').write_text(commands)
        obj = Path('build') / TARGET
        obj.unlink(missing_ok=True)
        call(['ccache', '--zero-stats'])
        debug = ROOT / ('probe-debug-' + mode)
        debug.mkdir()
        env = dict(os.environ, CCACHE_DEBUG='1', CCACHE_DEBUGDIR=str(debug),
                   CCACHE_LOGFILE=str(ROOT / ('probe-cache-' + mode + '.log')),
                   CCACHE_READONLY='1')
        if mode == 'shell_locale':
            env.pop('LC_CTYPE', None)
        started = time.monotonic()
        call(['cmake', '--build', 'build', '--target', TARGET, '--parallel', '4'], env=env)
        stats = capture(['ccache', '--show-stats'])
        print(stats, flush=True)
        Path('probe-stats-' + mode + '.txt').write_text(stats)
        record = next(r for r in json.loads(commands)
                      if r['output'] == TARGET)
        report[mode] = {'seconds': round(time.monotonic()-started, 3), 'compiler_command': record}
        shutil.copyfile('build/.ninja_log', 'probe-ninja-' + mode + '.log')
    report['compiler_commands_identical'] = report['pr']['compiler_command'] == report['baseline']['compiler_command']
finally:
    Path('tests/tests.cmake').write_text(original)
    Path('probe-cache-report.json').write_text(json.dumps(report, indent=2)+'\n')
    print('CACHE_PROBE_REPORT ' + json.dumps(report), flush=True)
