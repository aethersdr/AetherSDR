"""Exercise production admission with real signed release sizes and synthetic capacity."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
REPORT = json.loads((ROOT / 'report.json').read_text())
PREVIOUS = ROOT.parent / 'runtime-bypass-readiness'
PREVIOUS_REPORT = json.loads((PREVIOUS / 'report.json').read_text())
STAGE = ROOT / 'empty-budget-stage'
STAGE.mkdir(exist_ok=False)


def probe(arch, block, separate, available=1024**3, inodes=100000):
    directory = ROOT / 'short' / arch
    result = subprocess.run([str(ROOT / 'budget-build/budget-probe'), str(directory / 'candidate.release'),
        str(directory / 'public-key'), str(directory / 'release-digest'),
        '7' if arch == 'armv7' else '64', '4.2.18.41174' if arch == 'armv7' else '4.3.16.42034',
        str(STAGE), str(block), str(available), str(int(separate)), str(inodes),
        str(PREVIOUS / 'short' / arch / 'candidate.release'),
        str(PREVIOUS / 'short' / arch / 'public-key'),
        str(PREVIOUS / 'short' / arch / 'release-digest')],
        capture_output=True, text=True)
    assert result.returncode in (0, 1), result.stderr
    data = json.loads(result.stdout)
    assert data['accepted'] == (result.returncode == 0)
    return data


measurements = []
for arch, builds in REPORT['architectures'].items():
    release = builds['first']['release']
    sizes = {entry['name']: entry['bytes'] for entry in release['files']}
    for block in (1024, 4096):
        rounded = lambda size: (size + block - 1) // block * block
        permanent = sum(rounded(size) for name, size in sizes.items() if name not in ('payload.bundle', 'pd-update'))
        release_tree = rounded(432) + sum(map(rounded, sizes.values()))
        # Current terminal compaction removes only the unused payload. Its other
        # authenticated release members and both release headers remain.
        previous_sizes = {entry['name']: entry['bytes'] for entry in PREVIOUS_REPORT['architectures'][arch]['first']['release']['files']}
        previous_compacted = rounded(432) + sum(rounded(size) for name, size in previous_sizes.items() if name != 'payload.bundle')
        steady_pair = release_tree + previous_compacted
        state_allowance = 1024**2
        metadata_allowance = 64 * block
        fresh = permanent + rounded(sizes['payload.bundle']) + state_allowance + metadata_allowance
        successor = permanent + steady_pair + state_allowance + metadata_allowance
        for separate in (False, True):
            admitted = probe(arch, block, separate)
            assert admitted['accepted']
            need = max(v['required_bytes'] for v in admitted['volumes'])
            inode_need = max(v['required_inodes'] for v in admitted['volumes'])
            assert probe(arch, block, separate, available=need)['accepted']
            assert not probe(arch, block, separate, available=need - 1)['accepted']
            assert probe(arch, block, separate, inodes=inode_need)['accepted']
            assert not probe(arch, block, separate, inodes=inode_need - 1)['accepted']
            # Parent and stage share /user. The first callback is that volume;
            # with separate units the second volume is the root filesystem.
            store_need = admitted['volumes'][0]['required_bytes']
            total = successor + store_need
            measurements.append({'arch': arch, 'block_bytes': block, 'units_on_separate_filesystem': separate,
                'native_admission': admitted, 'boundary_checks_passed': ['exact byte', 'one byte short refused',
                    'exact inode', 'one inode short refused'], 'worker_size_charged': 512 * 1024,
                'permanent_management_files_allocated_bytes': permanent,
                'compacted_generation_allocated_bytes': steady_pair,
                'private_state_allowance_bytes': state_allowance, 'metadata_directory_allowance_bytes': metadata_allowance,
                'fresh_install_steady_budget_bytes': fresh, 'successor_steady_budget_bytes': successor,
                'successor_peak_including_native_reserve_bytes': total,
                'remaining_from_41_decimal_MB': 41000000 - total,
                'remaining_from_41_MiB': 41 * 1024**2 - total,
                'additional_one_release_upload_fits_41_decimal_MB': total + rounded(release['bytes']) <= 41000000})

report = {'production_ready': False, 'radio_used': False,
    'measurement': 'Native production upgrade-space functions, real authenticated release files; synthetic filesystem capacity and block sizes.',
    'source_commit': REPORT['source_commit'], 'previous_source_commit': PREVIOUS_REPORT['source_commit'], 'measurements': measurements,
    'limitations': ['Current installed dd33027f5 and new candidate authenticated under the same retained development publisher. Capacity is simulated, not a live storage measurement.',
        'Native admission is measured; steady-state storage is calculated from file inventory plus explicit allowances.',
        'Private state and metadata allowances are budget assumptions, not measured 6300 usage or enforced size caps.',
        '41 MB is a rounded user report: both decimal and binary interpretations are reported, not a current free-space measurement.',
        'No FLEX-6000 first-install adapter/carrier archive size is qualified.',
        'Full signed inputs must remain on tmpfs. Firmware-retained upload archives or other persistent copies are additional.',
        'Fresh first-install transient capsule/native-waveform overhead needs the final 6000 adapter; these results qualify upgrade admission only.']}
(ROOT / 'budget-report.json').write_text(json.dumps(report, indent=2) + '\n')
for row in measurements:
    print(row['arch'], row['block_bytes'], row['units_on_separate_filesystem'],
          'peak', row['successor_peak_including_native_reserve_bytes'],
          'decimal headroom', row['remaining_from_41_decimal_MB'])
