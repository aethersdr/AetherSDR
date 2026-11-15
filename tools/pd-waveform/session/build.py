#!/usr/bin/env python3
"""Build fixed session helpers as ordinary files; never installs privileges."""
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
    parser.add_argument('--probe-only', action='store_true')
    args = parser.parse_args()
    if args.test and args.probe_only:
        parser.error('--test and --probe-only are mutually exclusive')
    args.output.mkdir(parents=True, exist_ok=True)
    cache = args.output.resolve() / 'cache'
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(cache), ZIG_LOCAL_CACHE_DIR=str(cache))
    sources = {'pd-fixed-arp': 'pd_fixed_arp.c', 'pd-kernel-probe': 'kernel_probe.c',
               'pd-lease-guard': 'pd_lease_guard.c', 'pd-iq-stream': 'pd_iq_stream.c', 'pd-iq-source': 'pd_iq_source.c', 'pd-shim-launch': 'pd_shim_launch.c', 'pd-prepared-launch': 'pd_prepared_launch.c', 'pd-session-launch': 'pd_session_launch.c', 'pd-radio-launch': 'pd_radio_launch.c', 'pd-service-launch': 'pd_service_launch.c', 'pd-start-session': 'pd_start_session.c', 'pd-capture-bootstrap': 'pd_capture_bootstrap.c'}
    if args.test:
        sources = {'arp-watch-test': 'arp_watch_test.c', 'arp-journal-test': 'arp_journal_test.c', 'fixed-arp-test': 'fixed_arp_test.c', 'kernel-probe-test': 'kernel_probe_test.c', 'pi-probe-test': 'pi_probe_test.c',
                   'lease-guard-test': 'lease_guard_test.c', 'iq-stream-test': 'iq_stream_test.c', 'iq-source-test': 'iq_source_test.c', 'shim-launch-test': 'shim_launch_test.c', 'session-launch-test': 'session_launch_test.c', 'radio-launch-test': 'radio_launch_test.c', 'service-launch-test': 'service_launch_test.c', 'start-session-test': 'start_session_test.c', 'capture-test': 'capture_test.c'}
    elif args.probe_only:
        sources = {'pd-kernel-probe': 'kernel_probe.c'}
    if not args.probe_only:
        sources['primary-launch-test' if args.test else 'pd-primary-launch'] = (
            'primary_launch_test.c' if args.test else 'pd_primary_launch.c')
    for name, source in sources.items():
        transaction_sources = []
        transaction_flags = []
        if source in ('pd_start_session.c', 'start_session_test.c', 'pd_primary_launch.c', 'primary_launch_test.c'):
            transaction_sources = [HERE.parent / 'boot' / filename for filename in
                                   ('pd_transaction.c', 'pd_migration.c', 'pd_approval.c', 'pd_upgrade_approval.c', 'pd_upgrade_state.c')]
            transaction_flags = ['-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                                 '-I', str(HERE.parents[2] / 'third_party' / 'monocypher')]
        if source in ('pd_primary_launch.c','primary_launch_test.c'):
            transaction_sources += [HERE.parent / 'request/pd_request.c', HERE.parent / 'request/pd_upload.c',
                                    HERE.parent / 'boot/pd_transport.c', HERE.parents[2] / 'third_party/monocypher/monocypher.c']
            transaction_flags += ['-DPD_REQUEST_LIBRARY', '-I', str(HERE.parent / 'boot')]
        subprocess.run([
            'zig', 'cc', *TARGETS[args.arch], '-O2', '-static', '-s',
            '-Wall', '-Wextra', '-Werror', '-Wl,--build-id=none', '-pthread',
            *transaction_flags,
            str(HERE / source), *map(str, transaction_sources),
            *([str(HERE.parent / 'pd_local_iq.c'), str(HERE.parent / 'pd_stream_guard.c')] if source in ('pd_capture_bootstrap.c','capture_test.c') else []),
            '-I', str(HERE.parent), '-o', str(args.output / (name + '-' + args.arch)),
        ], env=env, check=True)


if __name__ == '__main__':
    main()
