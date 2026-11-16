#!/usr/bin/env python3
"""Extract bounded, credential-free facts from the saved cold-start cases.

This reads historical evidence only. It neither reproduces the failure nor
qualifies a current binary. The API tail is inspected without publishing it.
"""
import argparse
import hashlib
import json
from pathlib import Path


def summarize(path):
    raw = path.read_bytes()
    envelope = json.loads(raw)
    native = json.loads(envelope['logs'].splitlines()[0])
    rows = native['runs'][0]['status']
    keyed = [row for row in rows if row['keyed']]
    correcting = [row for row in keyed if row['correcting']]
    tail = native['api_status_tail']
    if isinstance(tail, str):
        tail = tail.splitlines()
    interlocks = [line for line in tail if '|interlock ' in line and 'state=' in line]
    stderr = native['managed_shim']['stderr']
    fields = ('seconds', 'keyed', 'calibration_state', 'calibration_samples_left',
              'correction_enabled', 'correcting', 'calibration_required', 'faults')
    return {
        'case': path.parent.name,
        'source_sha256': hashlib.sha256(raw).hexdigest(),
        'arm_reply': native['last_calibration_arm_reply'],
        'status_rows': len(rows),
        'keyed_rows': len(keyed),
        'first_keyed_status': {key: keyed[0][key] for key in fields if key in keyed[0]},
        'keyed_calibration_states': sorted({row['calibration_state'] for row in keyed}),
        'correcting_rows': len(correcting),
        'first_correcting_seconds': correcting[0]['seconds'] if correcting else None,
        'last_correcting_seconds': correcting[-1]['seconds'] if correcting else None,
        'public_fault_maxima': {
            key: max(row['faults'][key] for row in keyed)
            for key in keyed[0]['faults']
        },
        'native_start_message_captured': 'AetherPD 0.1.0 starting' in stderr,
        'native_retirement_record_captured': 'bounded source ended:' in stderr,
        'diagnostic_artifacts': sorted(native['diagnostics']),
        'api_tail_rows': len(tail),
        'api_tail_interlock_rows': len(interlocks),
        'api_tail_nonready_interlock_rows': sum('state=READY' not in line for line in interlocks),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence_root', type=Path)
    parser.add_argument('recovered_alignment_root', type=Path)
    args = parser.parse_args()
    cases = args.evidence_root / 'lp100a-bench-qualified' / 'cases'
    result = {
        'scope': 'Historical evidence extraction only; no RF, replay or current-candidate qualification',
        'cases': [summarize(cases / name / 'native.json') for name in (
            'production-pi-lp100a-5w-02', 'production-pi-lp100a-5w-03')],
        'conclusion': 'The failed source was canceled by the first keyed observation. The saved evidence does not identify the cancellation trigger.',
        'limits': [
            'Public zero fault counters do not exclude ownership, interlock, model-change or keying-admission cancellation.',
            'No native cancellation record or keyed interlock transition survives in these saved artifacts.',
            'The successful repeat is not a root-cause test or proof of binary equivalence.',
            'Further attribution requires a newly observed failure with the existing native retirement diagnostics and bounded admission/status history.',
        ],
    }
    previous = json.loads((args.evidence_root / 'alignment-slip-audit' / 'validation.json').read_text())
    recovered = []
    for case in previous['cases']:
        path = args.recovered_alignment_root / case['case'] / 'gui-rf-1-8600.json'
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != case['source_sha256']:
            raise SystemExit(f'Recovered source hash mismatch: {path}')
        recovered.append({
            'case': case['case'], 'recovered_path': str(path), 'source_sha256': digest,
            'matches_prior_audit': True, 'delay_change_samples': case['delay_change_samples'],
            'dispatch_trace_available': case['dispatch_trace_available'],
        })
    result['alignment_evidence_recovered'] = recovered
    result['alignment_conclusion'] = (
        'The original natural +128-sample event file survives byte-identically at a new path. '
        'It still lacks dispatch tracing; the injected-pause comparison does not identify the natural trigger.')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
