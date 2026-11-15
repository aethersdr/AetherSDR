#!/usr/bin/env python3
"""Build bounded native bundle verification/boot helpers; installs no privileges."""
import argparse
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import units
import worker_units
import upgrade_units

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TARGETS = {'host': [], 'arm64': ['-target', 'aarch64-linux-musl'],
           'armv7': ['-target', 'arm-linux-musleabihf', '-mcpu=cortex_a8']}


def build(output, arch, entry, public_key=None, sanitize=False, worker_test=False,
          management_release=None, upgrade_worker_test=False):
    output.mkdir(parents=True, exist_ok=True)
    zlib = ROOT / 'third_party/zlib'
    crypto = ROOT / 'third_party/monocypher'
    shutil.copyfile(zlib / 'zconf.h.included', output / 'zconf.h')
    (output / 'pd_units.h').write_text(''.join(
        'static const char* const pd_units_' + name + ('_managed' if managed else '') + '[4]={'
        + ','.join(json.dumps(value) for value in units.render(name, managed).values()) + '};\n'
        for name in ['armv7', 'arm64'] for managed in (False, True)))
    worker_definitions = worker_units.render()
    (output / 'pd_worker_unit_text.h').write_text(
        'static const char* const pd_worker_unit_names[2]={' + ','.join(map(json.dumps, worker_definitions)) + '};\n'
        + 'static const char* const pd_worker_unit_text[2]={' + ','.join(map(json.dumps, worker_definitions.values())) + '};\n')
    (output / 'pd_upgrade_unit_text.h').write_text(
        'static const char* const pd_upgrade_unit_names[2]={' + ','.join(map(json.dumps, upgrade_units.render('arm64'))) + '};\n'
        + ''.join('static const char* const pd_upgrade_unit_' + arch + '[2]={'
                  + ','.join(map(json.dumps, upgrade_units.render(arch).values())) + '};\n' for arch in ['arm64', 'armv7']))
    if public_key is not None:
        if len(public_key) != 32:
            raise ValueError('public key must be exactly32bytes')
        (output / 'pd_release_key.h').write_text('static const unsigned char pd_release_key[32]={'
            + ','.join(str(x) for x in public_key) + '};\n')
    if entry == 'pd_upgrade_worker.c':
        if management_release is None or public_key is None:
            raise ValueError('upgrade worker requires a signed managed release and explicit trust key')
        import management_blob
        verifier = build(output / 'verifier', 'host', 'release_check.c')
        management_blob.emit(management_release, public_key, 'arm64' if arch == 'host' else arch,
                             output / 'pd_management_embedded.h', verifier)
    elif management_release is not None or upgrade_worker_test:
        raise ValueError('management bytes are only accepted for the upgrade worker')
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(output / 'cache'), ZIG_LOCAL_CACHE_DIR=str(output / 'cache'))
    sources = [HERE / entry, HERE / 'pd_bootstrap_artifact.c', HERE / 'pd_bundle.c', HERE / 'pd_store.c', HERE / 'pd_release.c', HERE / 'pd_identity.c', HERE / 'pd_approval.c', HERE / 'pd_install.c', HERE / 'pd_state_transfer.c', crypto / 'monocypher.c', crypto / 'monocypher-ed25519.c']
    sources += [HERE / 'pd_migration.c', HERE / 'pd_transaction.c', HERE / 'pd_recovery.c', HERE / 'pd_capsule.c', HERE / 'pd_capsule_io.c', HERE / 'pd_worker_units.c', HERE / 'pd_unit_files.c', HERE / 'pd_upgrade_units.c', HERE / 'pd_management.c', HERE / 'pd_readiness.c', HERE / 'pd_primary.c', HERE / 'pd_service.c', HERE / 'pd_runtime.c', HERE / 'pd_installation.c', HERE / 'pd_upgrade.c', HERE / 'pd_upgrade_resume.c']
    sources += [HERE / 'pd_boot_primary.c', HERE / 'pd_boot_session.c', HERE / 'pd_boot_wait.c', HERE / 'pd_first.c']
    sources += [HERE.parent / 'request/pd_request.c', HERE.parent / 'request/pd_upload.c', HERE / 'pd_transport.c', HERE / 'pd_runtime_config.c']
    sources += [HERE / 'pd_generation.c', HERE / 'pd_retained.c', HERE / 'pd_retire.c', HERE / 'pd_slots_retire.c']
    sources += [HERE / 'pd_upgrade_capsule.c', HERE / 'pd_upgrade_bootstrap.c', HERE / 'pd_upgrade_approval.c', HERE / 'pd_upgrade_state.c', HERE / 'pd_upgrade_origin.c', HERE / 'pd_upgrade_space.c', HERE / 'pd_pair.c', HERE / 'pd_handoff.c']
    if entry != 'migration_check.c':
        sources += [HERE / 'pd_predecessor.c']
    sources += [zlib / (n + '.c') for n in ['inflate', 'inffast', 'inftrees', 'zutil', 'adler32', 'crc32']]
    if entry in ('pd_deliver.c', 'delivery_check.c'):
        sources += [HERE / 'pd_delivery.c']
    if entry == 'pd_deliver.c':
        sources += [HERE / 'pd_carrier.c', HERE / 'pd_endpoint.c']
    if entry == 'carrier_check.c':
        sources = [HERE / entry, HERE / 'pd_carrier.c']
    if entry in ('transport_check.c','pd_send.c'):
        sources = [HERE / entry, HERE / 'pd_transport.c', crypto / 'monocypher.c']
    if entry == 'pd_accept.c':
        sources = [HERE / entry, HERE / 'pd_transport.c', HERE / 'pd_endpoint.c', HERE / 'pd_runtime_config.c', HERE.parent / 'request/pd_upload.c', crypto / 'monocypher.c']
    if entry == 'runtime_config_check.c':
        sources = [HERE / entry, HERE / 'pd_runtime_config.c', HERE / 'pd_transport.c', HERE.parent / 'request/pd_upload.c', crypto / 'monocypher.c']
    if entry == 'bootstrap_artifact_check.c':
        sources = [HERE / entry, HERE / 'pd_bootstrap_artifact.c',
                   crypto / 'monocypher.c', crypto / 'monocypher-ed25519.c']
    compiler = ['cc'] if sanitize else ['zig', 'cc', *TARGETS[arch]]
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else ['-O2', '-static', '-s', '-Wl,--build-id=none']
    flags += ['-DPD_REQUEST_LIBRARY']
    if entry == 'installation_check.c':
        if arch != 'host':
            raise ValueError('installation status fixture is host-only')
        flags += ['-DPD_INSTALLATION_TEST', '-DPD_TRANSACTION_TEST', '-DPD_UPGRADE_TEST', '-DPD_STORE_TEST']
    if entry == 'runtime_config_check.c':
        flags += ['-DPD_RUNTIME_CONFIG_TEST']
    if entry == 'delivery_check.c':
        flags += ['-DPD_DELIVERY_TEST', '-Wl,--wrap=pd_identity_probe']
    if entry == 'boot_dispatch_check.c':
        flags += ['-Wl,--wrap=pd_boot_identity_wait']
    if entry in ['store_check.c', 'install_check.c']:
        flags += ['-DPD_STORE_TEST']
    if entry in ('upgrade_check.c','generation_check.c'):
        flags += ['-DPD_UPGRADE_TEST', '-DPD_STORE_TEST', '-DPD_TRANSACTION_TEST']
    if entry == 'generation_check.c':
        flags += ['-DPD_CAPSULE_TEST', '-DPD_CAPSULE_FAULTS']
    if entry == 'handoff_check.c':
        flags += ['-DPD_UPGRADE_TEST']
    if entry == 'install_check.c':
        flags += ['-DPD_INSTALL_TEST']
    if entry == 'approval_check.c':
        flags += ['-DPD_APPROVAL_TEST']
    if entry == 'state_check.c':
        flags += ['-DPD_STATE_FAULTS']
        if arch == 'host':
            flags += ['-DPD_STATE_TEST', '-DPD_STORE_TEST']
    if entry in ('transaction_check.c', 'upgrade_transaction_check.c') or (entry == 'session_gate_check.c' and arch == 'host'):
        flags += ['-DPD_TRANSACTION_TEST']
    if entry == 'upgrade_capsule_check.c':
        flags += ['-Wl,--wrap=pd_service_upgrade_cleanup_idle']
        flags += ['-DPD_CAPSULE_TEST', '-DPD_CAPSULE_FAULTS', '-DPD_TRANSACTION_TEST', '-DPD_WORKER_UNITS_FAULTS']
        if arch == 'host':
            flags += ['-DPD_WORKER_UNITS_TEST']
        if arch == 'host' and not sanitize:
            compiler = ['zig', 'cc', '-target', platform.machine() + '-linux-musl']
    if entry == 'capsule_check.c':
        flags += ['-DPD_CAPSULE_FAULTS', '-DPD_WORKER_UNITS_FAULTS']
        if arch == 'host':
            flags += ['-DPD_CAPSULE_TEST', '-DPD_TRANSACTION_TEST', '-DPD_WORKER_UNITS_TEST']
    if entry in ('worker_units_check.c','upgrade_units_check.c'):
        flags += ['-DPD_WORKER_UNITS_FAULTS']
        if arch == 'host':
            flags += ['-DPD_WORKER_UNITS_TEST', '-DPD_TRANSACTION_TEST']
    if entry == 'management_check.c':
        flags += ['-DPD_MANAGEMENT_TEST', '-DPD_UPGRADE_TEST', '-DPD_STORE_TEST', '-DPD_TRANSACTION_TEST']
        flags += ['-Wl,--wrap=fstat', '-Wl,--wrap=fstatfs']
    if entry == 'boot_wait_check.c':
        flags += ['-DPD_TRANSACTION_TEST']
        flags += ['-Wl,--wrap=' + name for name in ['clock_gettime', 'nanosleep', 'pd_identity_probe']]
    if entry == 'upgrade_units_check.c' and arch == 'host':
        flags += ['-DPD_CAPSULE_TEST']
    if entry == 'readiness_check.c':
        flags += ['-DPD_READINESS_FAULTS']
        if arch == 'host':
            flags += ['-DPD_READINESS_TEST', '-DPD_MIGRATION_TEST']
    if entry == 'runtime_check.c':
        flags += ['-DPD_RUNTIME_TEST', '-DPD_TRANSACTION_TEST']
    if entry in ('service_check.c', 'service_activation_check.c') and arch == 'host':
        flags += ['-DPD_SERVICE_TEST', '-DPD_WORKER_UNITS_TEST']
    if entry == 'service_activation_check.c':
        flags += ['-DPD_TRANSACTION_TEST']
        flags += ['-Wl,--wrap=' + name for name in ['pd_primary_open', 'pd_primary_wait', 'pd_primary_check', 'pd_primary_close']]
    if entry == 'predecessor_check.c' and arch == 'host':
        flags += ['-DPD_RECOVERY_TEST']
    if entry == 'restore_check.c':
        flags += ['-DPD_PREDECESSOR_TEST', '-DPD_RECOVERY_TEST', '-DPD_TRANSACTION_TEST', '-DPD_STATE_TEST', '-DPD_STORE_TEST']
    if entry == 'migration_check.c':
        flags += ['-DPD_MIGRATION_TEST', '-DPD_STATE_TEST', '-DPD_STORE_TEST']
    if worker_test:
        if arch != 'host' or entry != 'pd_install_worker.c':
            raise ValueError('worker fixture is host-only')
        if not sanitize:
            machine = platform.machine()
            if machine not in ('x86_64', 'aarch64'):
                raise ValueError('worker fixture requires a Linux x86_64/aarch64 host')
            compiler = ['zig', 'cc', '-target', machine + '-linux-musl']
        sources += [HERE / 'worker_fixture.c']
        flags += ['-DPD_WORKER_TEST', '-DPD_INSTALL_TEST', '-DPD_STORE_TEST', '-DPD_STATE_TEST',
                  '-DPD_RUNTIME_TEST', '-DPD_TRANSACTION_TEST', '-DPD_CAPSULE_FAULTS']
        wrapped = ['pd_identity_probe', 'pd_predecessor_probe', 'pd_predecessor_absent', 'pd_predecessor_image_verify',
                   'pd_predecessor_pin', 'pd_predecessor_stop', 'pd_predecessor_stopped',
                   'pd_predecessor_recover', 'pd_predecessor_recover_fresh', 'pd_predecessor_retire',
                   'pd_predecessor_restore', 'pd_readiness_probe', 'pd_readiness_wait',
                   'pd_service_reload', 'pd_service_worker_ready', 'pd_service_worker_launch',
                   'pd_service_prepare_runtime', 'pd_service_activate', 'pd_service_primary_autostart']
        flags += ['-Wl,--wrap=' + name for name in wrapped]
    if upgrade_worker_test:
        if arch != 'host' or sanitize:
            raise ValueError('upgrade worker namespace fixture requires a static host build')
        compiler = ['zig', 'cc', '-target', platform.machine() + '-linux-musl']
        sources += [HERE / 'upgrade_worker_fixture.c']
        flags += ['-DPD_UPGRADE_WORKER_TEST', '-DPD_UPGRADE_TEST', '-DPD_STORE_TEST',
                  '-DPD_RUNTIME_TEST', '-DPD_TRANSACTION_TEST', '-DPD_MANAGEMENT_TEST', '-DPD_CAPSULE_FAULTS']
        wrapped = ['pd_identity_probe', 'pd_service_runtime_quiet', 'pd_service_quiesce', 'pd_service_upgrade_cleanup_idle',
                   'pd_service_reload', 'pd_service_upgrade_worker_ready', 'pd_service_upgrade_prepare_runtime',
                   'pd_service_upgrade_activate', 'pd_service_primary_autostart', 'pd_service_upgrade_worker_launch',
                   'pd_service_read', 'pd_readiness_probe',
                   'pd_primary_open', 'pd_primary_wait', 'pd_primary_check', 'pd_primary_close']
        flags += ['-Wl,--wrap=' + name for name in wrapped]
    result = output / Path(entry).stem.replace('_', '-')
    subprocess.run([*compiler, *flags, '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-Wall', '-Wextra', '-Werror', '-I', str(output), '-I', str(HERE), '-I', str(crypto), '-I', str(zlib),
                    *map(str, sources), '-o', str(result)], env=env, check=True)
    if entry == 'pd_upgrade_worker.c' and result.stat().st_size > 512 * 1024:
        raise ValueError('upgrade worker exceeds the fixed capsule size bound')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--arch', choices=TARGETS, required=True)
    p.add_argument('--test', action='store_true')
    p.add_argument('--store-test', action='store_true')
    p.add_argument('--update', action='store_true')
    p.add_argument('--upgrade-test', action='store_true')
    p.add_argument('--upgrade-transaction-test', action='store_true')
    p.add_argument('--handoff-test', action='store_true')
    p.add_argument('--release-test', action='store_true')
    p.add_argument('--stage-release', action='store_true')
    p.add_argument('--install-release', action='store_true')
    p.add_argument('--install-test', action='store_true')
    p.add_argument('--identity-test', action='store_true')
    p.add_argument('--approval-test', action='store_true')
    p.add_argument('--state-test', action='store_true')
    p.add_argument('--predecessor-test', action='store_true')
    p.add_argument('--inspect-predecessor', action='store_true')
    p.add_argument('--migration-test', action='store_true')
    p.add_argument('--transaction-test', action='store_true')
    p.add_argument('--session-gate-test', action='store_true')
    p.add_argument('--capsule-test', action='store_true')
    p.add_argument('--upgrade-capsule-test', action='store_true')
    p.add_argument('--worker-units-test', action='store_true')
    p.add_argument('--readiness-test', action='store_true')
    p.add_argument('--primary-test', action='store_true')
    p.add_argument('--service-test', action='store_true')
    p.add_argument('--service-activation-test', action='store_true')
    p.add_argument('--installation-test', action='store_true')
    p.add_argument('--runtime-test', action='store_true')
    p.add_argument('--install-worker', action='store_true')
    p.add_argument('--upgrade-worker', action='store_true')
    p.add_argument('--management-release', type=Path)
    p.add_argument('--worker-fixture', action='store_true')
    p.add_argument('--restore-test', action='store_true')
    p.add_argument('--copy-predecessor', action='store_true')
    p.add_argument('--recover-predecessor', action='store_true')
    p.add_argument('--delivery-test', action='store_true')
    p.add_argument('--carrier-test', action='store_true')
    p.add_argument('--runtime-config-test', action='store_true')
    p.add_argument('--transport-test', action='store_true')
    p.add_argument('--send', action='store_true')
    p.add_argument('--accept', action='store_true')
    p.add_argument('--first-request', action='store_true')
    p.add_argument('--deliver', action='store_true')
    p.add_argument('--bootstrap-artifact-test', action='store_true')
    p.add_argument('--public-key', type=Path)
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    if a.sanitize and a.arch != 'host':
        p.error('sanitizers are host-only')
    if sum([a.first_request, a.installation_test, a.runtime_config_test, a.accept, a.send, a.transport_test, a.carrier_test, a.deliver, a.delivery_test, a.bootstrap_artifact_test, a.test, a.store_test, a.update, a.upgrade_test, a.upgrade_transaction_test, a.handoff_test, a.release_test, a.stage_release, a.install_release, a.install_test, a.identity_test, a.approval_test, a.state_test, a.predecessor_test, a.inspect_predecessor, a.migration_test, a.transaction_test, a.session_gate_test, a.capsule_test, a.upgrade_capsule_test, a.worker_units_test, a.readiness_test, a.primary_test, a.service_test, a.service_activation_test, a.runtime_test, a.install_worker, a.upgrade_worker, a.restore_test, a.copy_predecessor, a.recover_predecessor]) > 1:
        p.error('choose one entry point')
    if not (a.installation_test or a.runtime_config_test or a.accept or a.send or a.transport_test or a.carrier_test or a.delivery_test or a.bootstrap_artifact_test or a.test or a.store_test or a.upgrade_test or a.upgrade_transaction_test or a.release_test or a.install_test or a.identity_test or a.approval_test or a.state_test or a.predecessor_test or a.inspect_predecessor or a.migration_test or a.transaction_test or a.session_gate_test or a.capsule_test or a.upgrade_capsule_test or a.worker_units_test or a.readiness_test or a.primary_test or a.service_test or a.service_activation_test or a.runtime_test or a.restore_test) and not a.public_key:
        p.error('boot builds require an explicitly supplied release public key; no default test key')
    entry = 'state_check.c' if a.state_test else 'approval_check.c' if a.approval_test else 'pd_install_release.c' if a.install_release else 'install_check.c' if a.install_test else 'identity_test.c' if a.identity_test else 'pd_stage_release.c' if a.stage_release else 'release_check.c' if a.release_test else 'store_check.c' if a.store_test else 'bundle_check.c' if a.test else 'pd_update.c' if a.update else 'pd_boot.c'
    if a.installation_test:
        entry = 'installation_check.c'
    elif a.runtime_config_test:
        if a.arch!='host':p.error('runtime config fixture is host-only')
        entry='runtime_config_check.c'
    elif a.first_request:
        if a.arch!='arm64':p.error('first request is currently ARM64-only')
        entry = 'pd_first_request.c'
    elif a.accept:
        entry = 'pd_accept.c'
    elif a.send:
        entry = 'pd_send.c'
    elif a.transport_test:
        entry = 'transport_check.c'
    elif a.carrier_test:
        entry = 'carrier_check.c'
    elif a.deliver:
        entry = 'pd_deliver.c'
    elif a.delivery_test:
        entry = 'delivery_check.c'
    elif a.bootstrap_artifact_test:
        entry = 'bootstrap_artifact_check.c'
    elif a.upgrade_transaction_test:
        entry = 'upgrade_transaction_check.c'
    elif a.handoff_test:
        entry = 'handoff_check.c'
    elif a.upgrade_test:
        entry = 'upgrade_check.c'
    elif a.predecessor_test:
        entry = 'predecessor_check.c'
    elif a.inspect_predecessor:
        entry = 'pd_inspect_predecessor.c'
    elif a.migration_test:
        entry = 'migration_check.c'
    elif a.transaction_test:
        entry = 'transaction_check.c'
    elif a.session_gate_test:
        entry = 'session_gate_check.c'
    elif a.upgrade_capsule_test:
        entry = 'upgrade_capsule_check.c'
    elif a.capsule_test:
        entry = 'capsule_check.c'
    elif a.worker_units_test:
        entry = 'worker_units_check.c'
    elif a.readiness_test:
        entry = 'readiness_check.c'
    elif a.install_worker:
        entry = 'pd_install_worker.c'
    elif a.upgrade_worker:
        entry = 'pd_upgrade_worker.c'
    elif a.runtime_test:
        entry = 'runtime_check.c'
    elif a.service_activation_test:
        entry = 'service_activation_check.c'
    elif a.service_test:
        entry = 'service_check.c'
    elif a.primary_test:
        entry = 'primary_check.c'
    elif a.restore_test:
        entry = 'restore_check.c'
    elif a.copy_predecessor:
        entry = 'pd_copy_predecessor.c'
    elif a.recover_predecessor:
        entry = 'pd_recover_predecessor.c'
    build(a.output.resolve(), a.arch, entry,
          a.public_key.read_bytes() if a.public_key else None, a.sanitize,
          a.worker_fixture and not a.upgrade_worker, a.management_release,
          a.worker_fixture and a.upgrade_worker)


if __name__ == '__main__':
    main()
