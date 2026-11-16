"""Build the current production analyzer and compare all four retained RF exports."""
import base64
import hashlib
import json
from pathlib import Path
import subprocess

p = Path(__file__).resolve().parent
w = p.parents[1] / 'aetherpd-source'
sources = [w/'src/core/txlinearity'/name for name in ('ImdAnalyzer.cpp', 'TxSpectrumAnalyzer.cpp', 'SpectrumWindow.cpp')]
sources.append(w/'src/core/dsp/FftwPlannerLock.cpp')
cmd = ['g++', '-std=c++20', '-O2', '-I', str(w/'src'), str(p/'analyze-raw.cpp'), *map(str, sources), '-lfftw3', '-pthread', '-o', str(p/'analyze-raw')]
subprocess.run(cmd, check=True)
r = {'build_command': cmd, 'source_commit': subprocess.check_output(['git', '-C', str(w), 'rev-parse', 'HEAD'], text=True).strip(),
     'production_sources_sha256': {str(f.relative_to(w)): hashlib.sha256(f.read_bytes()).hexdigest() for f in sources}, 'cases': [], 'negative_cases': {}}
for watts in (5, 10):
    for mode in ('off', 'on'):
        export_path = p.parent/f'candidate-rf/{watts}w/{mode}-01.json'
        exported = json.loads(export_path.read_text())
        raw = b''.join(base64.b64decode(c['iq_f32le'], validate=True) for c in exported['captures'])
        path = p/f'check-{watts}-{mode}.f32le'
        path.write_bytes(raw)
        result = json.loads(subprocess.check_output([str(p/'analyze-raw'), str(path)], text=True))
        assert len(result['products']) == 6 and not result['clipping_known']
        errors = []
        for current, original in zip(result['products'], exported['measurement']['products']):
            assert (current['order'], current['upper']) == (original['order'], original['upper'])
            assert current['noise_limited'] == original['noise_limited']
            errors.append(abs(current['dbc_per_tone']-original['dbc_per_tone']))
        assert max(errors) < 1e-8
        r['cases'].append({'watts': watts, 'mode': mode, 'export_sha256': hashlib.sha256(export_path.read_bytes()).hexdigest(), 'iq_sha256': hashlib.sha256(raw).hexdigest(), 'max_product_error_db': max(errors), 'result': result})
for name, data in [('truncated', raw[:-1]), ('nonfinite', b'\x00\x00\xc0\x7f'+raw[4:]), ('zero', bytes(len(raw)))]:
    path = p/('negative-'+name+'.f32le')
    path.write_bytes(data)
    run = subprocess.run([str(p/'analyze-raw'), str(path)], capture_output=True, text=True)
    assert run.returncode != 0
    r['negative_cases'][name] = {'exit': run.returncode, 'stderr': run.stderr}
    path.unlink()
r['binary_sha256'] = hashlib.sha256((p/'analyze-raw').read_bytes()).hexdigest()
(p/'analyzer-check.json').write_text(json.dumps(r, indent=2)+'\n')
print(json.dumps({'comparisons':len(r['cases']), 'largest_difference_db': max(c['max_product_error_db'] for c in r['cases']), 'negative_cases':r['negative_cases']}))
