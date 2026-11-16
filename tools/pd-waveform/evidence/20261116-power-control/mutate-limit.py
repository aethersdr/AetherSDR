from pathlib import Path
import hashlib, json, os, subprocess
root = Path('/home/jeremy/AetherSDR/build/aetherpd-source')
build = root.parent / 'aetherpd-shim-integration/first-install-desktop-monitor/project'
out = Path(__file__).resolve().parent
source = root / 'src/models/AetherPDMeasurementModel.cpp'
original = source.read_bytes()
needle = b' || nominalWatts > *control.maximum'
assert original.count(needle) == 1
env = dict(os.environ, QT_QPA_PLATFORM='offscreen', CCACHE_DIR=str(build.parent/'ccache'))
def run(command, name):
    with (out/name).open('w') as log:
        return subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
build_command = ['cmake', '--build', str(build), '--target', 'aetherpd_dialog_test', '-j', '4']
result = {}
try:
    source.write_bytes(original.replace(needle, b''))
    assert run(build_command, 'mutation-build.log') == 0
    code = run(['ctest', '--test-dir', str(build), '-R', '^aetherpd_dialog_test$', '--output-on-failure'], 'mutation-test.log')
    expected = 'FAIL: foreign session, out-of-range and unchanged power requests send nothing'
    result = {'mutation': 'Remove reported maximum-power guard', 'exit_code': code,
              'expected_assertion_failed': expected in (out/'mutation-test.log').read_text()}
    assert code != 0 and result['expected_assertion_failed']
finally:
    source.write_bytes(original)
    result['restored_source_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
    result['restore_build_exit_code'] = run(build_command, 'restore-build.log')
    (out/'mutation.json').write_text(json.dumps(result, indent=2)+'\n')
    assert result['restore_build_exit_code'] == 0
