"""Receive-only qualification of installed IQ capture with stock APD selected."""
import base64
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time

p = Path(__file__).resolve().parent
sys.path.insert(0, str(p.parents[1] / 'aetherpd-source/tools'))
from automation_probe import Bridge

output = p / 'receive-probe.json'
assert not output.exists()
b = Bridge('/tmp/aetherpd-partial-release')
r = {'RF': False, 'commands': [], 'captures': {}}

def req(q):
    result = b.request(q, 6)
    r['commands'].append({'request': q, 'response': result})
    assert result['ok'], result
    return result

def invoke(target, action='click', value=None):
    q = {'cmd': 'invoke', 'target': target, 'action': action}
    if value is not None:
        q['value'] = value
    return req(q)

def api(path):
    q = subprocess.run([str(p.parent / 'first-iq-live/vault-v3'), path], capture_output=True, text=True, timeout=23)
    result = json.loads(q.stdout)
    if path == 'aetherpd/status' and result['http_status'] == 503 and result['body'].get('state') in ('starting', 'unavailable'):
        return result['body']
    assert q.returncode == 0 and result['http_status'] == 200, result
    return result['body']

def wait(f):
    end = time.monotonic() + 15
    while True:
        value = f()
        if value:
            return value
        assert time.monotonic() < end, 'Receive-only prerequisite timeout'
        time.sleep(.2)

def capture(name):
    status = api('aetherpd/status')
    assert status['registered'] and not status['keyed'] and not status['correction_enabled']
    manifest = api('aetherpd/capture')
    assert manifest['sample_count'] == 16384 and manifest['sample_rate_hz'] == 48000 and not manifest['keyed']
    assert manifest['feedback_binding']['valid'] and manifest['feedback_binding']['channel'] == 4
    assert manifest['feedback_binding']['transport'] == 'local_dax_iq'
    raw = bytearray()
    for i in range(32):
        chunk = api('aetherpd/capture/' + manifest['capture_id'] + '/' + str(i))
        assert chunk['offset'] == 512*i
        data = base64.b64decode(chunk['iq_f32le'], validate=True)
        assert len(data) == 4096
        raw.extend(data)
    values = struct.unpack('<32768f', raw)
    assert all(math.isfinite(v) for v in values) and any(values)
    (p / (name + '.f32le')).write_bytes(raw)
    r['captures'][name] = {'status': status, 'manifest': manifest, 'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw)}

try:
    r['before'] = req({'cmd': 'get', 'model': 'radio'})['radio']
    assert r['before']['connected'] and not r['before']['transmitting'] and not r['before']['fullDuplex']
    assert r['before']['serial'] == '2125-1213-8600-8895'
    if sys.argv[1:] == ['--resume-existing']:
        assert api('aetherpd/status').get('registered')
        r['resumed_same_verified_session'] = True
    else:
        assert not sys.argv[1:]
        assert api('aetherpd/status')['state'] == 'stopped'
        invoke('pdStart')
    wait(lambda: api('aetherpd/status').get('registered'))
    req({'cmd': 'slice', 'action': 'mode', 'value': 'PD', 'id': '0'})
    wait(lambda: req({'cmd': 'get', 'model': 'slices'})['slices'][0]['mode'] == 'PD')
    time.sleep(.5)
    if api('aetherpd/status')['correction_enabled']:
        invoke('pdBypassCorrection')
    wait(lambda: not api('aetherpd/status')['correction_enabled'])
    req({'cmd': 'slice', 'action': 'mode', 'value': 'USB', 'id': '0'})
    wait(lambda: req({'cmd': 'get', 'model': 'slices'})['slices'][0]['mode'] == 'USB')
    capture('stock-off-receive')
    invoke('APD pre-distortion', 'setChecked', 'true')
    wait(lambda: req({'cmd': 'get', 'model': 'transmit'})['transmit']['apdEnabled'])
    wait(lambda: api('aetherpd/status')['state'] == 'blocked_by_radio_apd')
    capture('stock-on-receive')
    r['passed'] = True
except BaseException as error:
    r['error'] = repr(error)
    raise
finally:
    try:
        # This prepares mutual exclusion for the next test; it does not restore
        # saved processing settings or key the transmitter.
        invoke('APD pre-distortion', 'setChecked', 'false')
        wait(lambda: not req({'cmd': 'get', 'model': 'transmit'})['transmit']['apdEnabled'])
        if api('aetherpd/status')['state'] != 'stopped':
            invoke('pdStop')
        wait(lambda: api('aetherpd/status')['state'] == 'stopped')
        r['after'] = req({'cmd': 'get', 'model': 'radio'})['radio']
        assert not r['after']['transmitting']
        r['session_stopped'] = True
    finally:
        output.write_text(json.dumps(r, indent=2) + '\n')
        b.close()
print(json.dumps({'passed': r.get('passed', False), 'RF': False, 'captures': list(r['captures'])}))
