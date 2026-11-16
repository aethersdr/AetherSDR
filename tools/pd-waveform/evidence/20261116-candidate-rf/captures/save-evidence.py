"""Archive finite matched RF evidence without changing radio state."""
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

p = pathlib.Path(__file__).resolve().parent
w = p.parents[1] / 'aetherpd-source'
pd = w / 'tools/pd-waveform'
dest = pd / 'evidence/20261116-candidate-rf'

def read(path):
    return json.loads(path.read_text())

def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + '.new')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)

assert subprocess.check_output(['git', '-C', str(w), 'rev-parse', 'HEAD'], text=True).strip() == '3e73dcfcbaa98b9e61fe29bc3778bbc2fd2efa1c'
assert read(p / 'stopped.json')['status']['body']['state'] == 'stopped'
audit_path = p.parent / 'guardian-owner-live/candidate-rf-idle-result.json'
audit = read(audit_path)
host = json.loads(audit['logs'])
assert host['passed'] and audit['job_container_deleted'] and audit['temporary_host_directory_deleted']
pairs = []
for watts in (5, 10):
    root = p / f'{watts}w'
    comparison = read(root / 'comparison.json')
    pair = {'nominal_watts': watts}
    for mode in ('off', 'on'):
        exported = read(root / f'{mode}-01.json')
        events = read(root / f'{mode}-01-events.json')
        assert exported['measurement']['valid'] and len(exported['captures']) == 4
        assert exported['correction_active'] == (mode == 'on')
        intents = [e for e in events if e['event'] == 'operator_key_intent']
        assert len(intents) == 1 and events[-1]['event'] == 'idle_session_held'
        outcome = next(e for e in events if e['event'] == 'measurement_outcome')
        assert outcome['saw_key'] and not outcome['service']['keyed']
        assert not outcome['widgets']['pdMeasureStop']['enabled']
        assert all(v == 0 for v in outcome['service']['faults'].values())
        assert outcome['service']['input_limited_samples'] == 0
        observations = [e for e in events if e['event'] == 'bridge' and e['request'] == {'cmd':'get','model':'radio'} and e['elapsed_s'] > intents[0]['elapsed_s']]
        first_key = next(e for e in observations if e['result']['radio']['transmitting'])
        idle = next(e for e in observations if e['elapsed_s'] > first_key['elapsed_s'] and not e['result']['radio']['transmitting'])
        pair[mode] = {
            'four_frames_complete': True,
            'intent_to_observed_receive_seconds': round(idle['elapsed_s'] - intents[0]['elapsed_s'], 3),
            'sampled_radio_peak_forward_watts': max(e['result']['radio']['txPower'] or 0 for e in observations),
            'faults_at_completion': outcome['service']['faults'],
            'input_limited_samples': outcome['service']['input_limited_samples'],
            'release_confirmed': True,
        }
    pair.update(
        imd3_off_dbc_per_tone=[v['dbc_per_tone'] for v in comparison['baseline']['products'] if v['order'] == 3],
        imd3_on_dbc_per_tone=[v['dbc_per_tone'] for v in comparison['products'] if v['order'] == 3],
        imd3_improvement_db=[v['per_tone'] for v in comparison['improvement_db'] if v['order'] == 3],
        fundamental_power_difference_db=comparison['fundamental_power_difference_db'],
        numerical_max_error_db=max(comparison['max_spectrum_error_db'], comparison['baseline']['max_spectrum_error_db']),
    )
    pairs.append(pair)

dest.mkdir(parents=True, exist_ok=True)
for root, prefix in ((p, pathlib.Path('captures')), (p.parent/'radio-return-check', pathlib.Path('receive-check'))):
    for source in sorted(root.rglob('*')):
        if not source.is_file() or '__pycache__' in source.parts:
            continue
        target = dest / prefix / source.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
for name in ('candidate-rf-idle-job.py', 'candidate-rf-idle-result.json'):
    shutil.copyfile(p.parent/'guardian-owner-live'/name, dest/name)
manifest = {}
for f in sorted(dest.rglob('*')):
    if not f.is_file():
        continue
    if f.suffix in ('.json', '.py', '.log'):
        text = f.read_text()
        assert not re.search(r'-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----|Bearer [A-Za-z0-9._~-]{16,}|"auth_token"\s*:\s*"[^\"]+"', text), str(f)
    manifest[str(f.relative_to(dest))] = hashlib.sha256(f.read_bytes()).hexdigest()
now = datetime.datetime.now(datetime.timezone.utc).isoformat()
report = {
    'schema': 1, 'updated_utc': now, 'production_ready': False,
    'status': 'One matched finite OFF/ON pair at each nominal setting passed on the exact installed candidate',
    'runtime_source': 'e8d68ad4bcfca1bf87f1d95b6c613cda17fac097',
    'desktop_source': 'b1256a1657319360279029cabfcbcb647728b86a',
    'desktop_sha256': read(p/'instance.json')['binary_sha256'],
    'radio': 'FLEX-8600', 'firmware': '4.3.16.42034',
    'configuration': {'center_mhz': 7.15, 'tx_antenna': 'ANT1', 'feedback': 'RX_A', 'preamp_db': 0, 'FDX': False, 'transport': 'local_dax_iq', 'channel': 4, 'configured_max_watts': 10, 'source_seconds': 8, 'authorization_seconds': 10, 'LP100A': 'deferred'},
    'pairs': pairs,
    'preflight_refusal': 'First 10 W corrected invocation refused before any bridge/radio command because the 31-second cooldown had not elapsed. Preserved separately; explicitly invoked after cooldown. No guard changed.',
    'receive_recheck': 'Expected release and fresh 16384-sample IQ capture verified after operator reported radio back. This alone does not establish a new reboot.',
    'cleanup': {'radio_idle': True, 'session_stopped': True, 'ARP_absent': not host['arp'], 'accept_local': host['accept_local'], 'processes': host['processes'], 'runtime_hashes_verified': True, 'temporary_audit_removed': True, 'processing_settings_restored': False},
    'limits': [
        'One finite pair per power setting; not sustained, speech, all-band or actual FLEX-6300 qualification.',
        '5 W OFF baseline predates the operator radio-back message; fresh receive check verified the same release, GUI binding and configuration before ON. No continuous environmental stability claim.',
        'Raw-IQ numerical agreement is not an independent RF instrument. Absolute watts and ADC headroom remain uncalibrated.',
        'Polled radio receive times are not calibrated physical RF cessation times.',
        'Stock APD comparison on this candidate remains pending. Existing stock comparison belongs to an earlier build and different capture/analysis protocol.',
        'Historical alignment/cold-start attribution, remaining process faults, exact-candidate interrupted update/rollback and power loss, ARMv7 first-install adapter, actual6300 and independent release gates remain open.',
    ],
    'evidence_directory': str(dest.relative_to(pd)), 'evidence_sha256': manifest,
}
atomic_json(pd/'CANDIDATE-RF-QUALIFICATION.json', report)
progress = read(pd/'PROGRESS.json')
progress['updated_utc'] = now
progress['current_focus'] = 'Exact installed runtime and desktop passed finite matched 5/10 W OFF/ON measurements using local DAX IQ with FDX disabled. Raw IQ independently recomputed; session stopped and cleanup audited.'
progress['next_action'] = 'Prepare matched final-candidate stock APD capture using a separate receive-only measurement path; preserve mutual exclusion with AetherPD correction. Then continue sustained/speech, remaining process faults, historical failure attribution, installation/6300 and independent release gates.'
progress['candidate_rf'] = {'report': 'CANDIDATE-RF-QUALIFICATION.json', 'pairs': pairs, 'stock_comparison_complete': False, 'production_ready': False}
delivery = progress['current_delivery_status']
delivery['latest_live_report'] = 'CANDIDATE-RF-QUALIFICATION.json'
delivery['installed_state'] = 'Runtime e8d68ad4b with desktop b1256a165; finite matched 5/10 W pairs passed, radio idle and native session stopped, cleanup audited.'
delivery['source_provenance_note'] = 'Latest matched pairs are attributed to runtime e8d68ad4b and desktop b1256a165. Older reports retain their own source attribution; stock APD comparison is not yet repeated on this candidate.'
atomic_json(pd/'PROGRESS.json', progress)
print(json.dumps({'report': 'CANDIDATE-RF-QUALIFICATION.json', 'files': len(manifest), 'pairs': pairs}))
