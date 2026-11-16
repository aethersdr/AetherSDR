import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil

p = Path(__file__).resolve().parent
pd = p.parents[1]/'aetherpd-source/tools/pd-waveform'
dest = pd/'evidence/20261116-stock-receive'
read = lambda f: json.loads(f.read_text())
probe = read(p/'receive-probe.json')
analysis = read(p/'analyzer-check.json')
audit_path = p.parent/'guardian-owner-live/stock-receive-idle-result.json'
audit = read(audit_path)
host = json.loads(audit['logs'])
assert probe['passed'] and probe['session_stopped'] and not probe['after']['transmitting']
assert host['passed'] and audit['job_container_deleted'] and audit['temporary_host_directory_deleted']
assert len(analysis['cases']) == 4 and len(analysis['negative_cases']) == 3
for name, capture in probe['captures'].items():
    assert hashlib.sha256((p/(name+'.f32le')).read_bytes()).hexdigest() == capture['sha256']
    assert capture['status']['correction_enabled'] is False
    assert capture['bytes'] == 131072 and not capture['manifest']['keyed']
names = ['receive-probe.py', 'receive-probe.json', 'stock-off-receive.f32le', 'stock-on-receive.f32le',
         'startup-poll-refusal-receive-probe.py', 'receive-probe-first-attempt.json',
         'idle-mode-refusal.py', 'idle-mode-refusal.json',
         'state-label-refusal-receive-probe.py', 'receive-probe-state-label-refusal.json',
         'state-label-refusal-stock-off-receive.f32le',
         'analyze-raw.cpp', 'check-analyzer.py', 'analyzer-check.json', 'save-receive-evidence.py']
dest.mkdir(parents=True, exist_ok=False)
for name in names:
    shutil.copyfile(p/name, dest/name)
for name in ('stock-receive-idle-job.py', 'stock-receive-idle-result.json'):
    shutil.copyfile(p.parent/'guardian-owner-live'/name, dest/name)
manifest = {}
for f in dest.iterdir():
    if f.suffix != '.f32le':
        assert not re.search(r'-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----|Bearer [A-Za-z0-9._~-]{16,}', f.read_text())
    manifest[f.name] = hashlib.sha256(f.read_bytes()).hexdigest()
now = datetime.datetime.now(datetime.timezone.utc).isoformat()
report = {
    'updated_utc': now, 'production_ready': False, 'RF_transmitted': False,
    'runtime_source': 'e8d68ad4bcfca1bf87f1d95b6c613cda17fac097',
    'desktop_source': 'b1256a1657319360279029cabfcbcb647728b86a',
    'status': 'Installed local IQ capture works receive-only in USB with stock APD selected and AetherPD explicitly bypassed',
    'radio': {'model': 'FLEX-8600', 'firmware': '4.3.16.42034', 'frequency_mhz': 7.15, 'FDX': False, 'feedback': 'RX_A', 'IQ_channel': 4},
    'captures': probe['captures'],
    'analysis': {'production_code_used': True, 'report': 'analyzer-check.json', 'existing_exports_checked': 4,
                 'max_product_difference_db': max(c['max_product_error_db'] for c in analysis['cases']),
                 'negative_cases': analysis['negative_cases'], 'absolute_watts_calibrated': False, 'ADC_headroom_known': False},
    'bench_script_failures_preserved': [
        'Initial script treated expected HTTP 503 starting as terminal. Re-poll proved the same session live; it was resumed rather than restarted.',
        'Bypass request needs an active PD slice. The bench script was corrected to select PD while idle before bypass, then switch USB.',
        'Public state prioritizes explicit bypass: stock APD enabled plus correction disabled reports disabled, not blocked_by_radio_apd. Assertion corrected against aetherpd.go; no product code or interlock changed.'
    ],
    'cleanup': {'radio_idle': True, 'stock_APD_off': True, 'AetherPD_session_stopped': True, 'ARP_absent': not host['arp'],
                'accept_local': host['accept_local'], 'remaining_processes': len(host['processes']),
                'runtime_hashes_verified': True, 'all_remaining_processes_UID78666_caps_zero_NNP1': True,
                'temporary_audit_removed': True, 'processing_settings_restored': False},
    'remaining_comparison_steps': [
        'Prepare a finite PC-audio source with the same 710/1900 Hz tones, explicit GUI keying and the installed bounded unkey helper. No DAX audio feedback.',
        'Confirm the audio route and stock sampler RX_A readback without RF; retain the 10 W configured maximum and existing bench power checks.',
        'Take bounded stock OFF and ON captures; require observed stock equalizer activity before attributing correction. Label cached/warm versus fresh acquisition.',
        'Collect four complete 16384-sample frames at 48 kHz for each result and analyze using this production BH7/IMD code. Reject mismatched output power, invalid ownership, incomplete frames or source changes.',
        'Report current stock and AetherPD results with their exact sources, drive, timing and power-matching limits. Repeat rather than substitute the older stock report if a current match is unavailable.'
    ],
    'limits': ['Receive-only stock selection is not proof stock APD corrects during TX.',
               'Offline reader agreement verifies use of the same analyzer, not an independent calibrated RF measurement.',
               'No RF comparison, sustained speech or broader production gate is closed by this report.'],
    'evidence_directory': str(dest.relative_to(pd)), 'evidence_sha256': manifest,
}
def save(path, value):
    temp = path.with_suffix('.json.new')
    temp.write_text(json.dumps(value, indent=2)+'\n')
    temp.replace(path)
save(pd/'STOCK-RECEIVE-QUALIFICATION.json', report)
progress = read(pd/'PROGRESS.json')
progress['updated_utc'] = now
progress['current_focus'] = 'Stock-comparison prerequisite passed: installed local DAX IQ captures in USB with stock APD selected and AetherPD bypassed. The same production analyzer reproduces all four current RF exports. No stock RF comparison yet.'
progress['next_action'] = 'Prepare finite PC-audio stock tone source, installed bounded unkey control and read-only APD telemetry; then bounded stock OFF/ON RF captures with matched output and the same analyzer. Preserve all remaining P1-P10 gates.'
progress['stock_receive'] = {'report': 'STOCK-RECEIVE-QUALIFICATION.json', 'receive_only_passed': True, 'RF_comparison_complete': False}
save(pd/'PROGRESS.json', progress)
print(json.dumps({'report': 'STOCK-RECEIVE-QUALIFICATION.json', 'evidence_files': len(manifest), 'RF': False}))
