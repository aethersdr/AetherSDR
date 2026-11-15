#!/usr/bin/env python3
"""Build the fixed FLEX-8600 capture constructor; installs no privileges."""
import argparse
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
TARGETS = {
    'host': [],
    'arm64': ['-target', 'aarch64-linux-musl'],
    'armv7': ['-target', 'arm-linux-musleabihf', '-mcpu=cortex_a8'],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--arch', choices=TARGETS, required=True)
    parser.add_argument('--test', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cache = args.output.resolve() / 'cache'
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(cache), ZIG_LOCAL_CACHE_DIR=str(cache))
    source = 'capture_test.c' if args.test else 'pd_capture_bootstrap.c'
    name = 'capture-test' if args.test else 'pd-capture-bootstrap'
    subprocess.run([
        'zig', 'cc', *TARGETS[args.arch], '-O2', '-static', '-s',
        '-Wall', '-Wextra', '-Werror', '-I', str(HERE.parent),
        str(HERE / source), str(HERE.parent / 'pd_local_iq.c'),
        str(HERE.parent / 'pd_stream_guard.c'), '-o',
        str(args.output / (name + '-' + args.arch)),
    ], check=True, env=env)


if __name__ == '__main__':
    main()
