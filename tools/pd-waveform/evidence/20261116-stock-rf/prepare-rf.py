"""Receive-only stock-comparison preparation and bounded-helper checks."""
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import time

p = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('common',p/'bench-common.py')
c = importlib.util.module_from_spec(spec); spec.loader.exec_module(c)
out = p/'rf-prepared.json'; assert not out.exists()
r = {'RF':False}
radio = c.bridge({'cmd':'get','model':'radio'})['radio']
assert radio['connected'] and not radio['transmitting'] and not radio['fullDuplex']
assert radio['serial']=='2125-1213-8600-8895'
assert c.api('status')['body']['state']=='stopped'
c.invoke('pdStart')
c.wait(lambda:c.api('status',transient=True)['body'].get('registered'))
c.bridge({'cmd':'slice','action':'mode','value':'PD','id':'0'})
c.wait(lambda:c.bridge({'cmd':'get','model':'slices'})['slices'][0]['mode']=='PD')
time.sleep(.5)
if c.api('status')['body']['correction_enabled']:
    c.invoke('pdBypassCorrection')
c.wait(lambda:not c.api('status')['body']['correction_enabled'])
c.bridge({'cmd':'slice','action':'mode','value':'USB','id':'0'})
c.wait(lambda:c.bridge({'cmd':'get','model':'slices'})['slices'][0]['mode']=='USB')
c.invoke('Microphone gain','setValue','35')
c.invoke('APD pre-distortion','setChecked','false')
r['models'] = {m:c.bridge({'cmd':'get','model':m}) for m in ('radio','transmit','slices','pans','audio','dsp')}
tx = r['models']['transmit']['transmit']; assert tx['rfPower']==10 and tx['maxPowerLevel']==10 and not tx['apdEnabled'] and tx['micSelection']=='PC'
assert not tx['speechProc'] and not tx['voxEnable'] and not tx['dax']
sl = r['models']['slices']['slices']; assert len(sl)==1 and sl[0]['mode']=='USB' and sl[0]['frequency']==7.15 and sl[0]['txAntenna']=='ANT1' and sl[0]['rxAntenna']=='RX_A'
pan = r['models']['pans']['pans']; assert len(pan)==1 and pan[0]['rfGain']==0 and pan[0]['centerMhz']==7.15
observer = c.Observer()
try:
    r['observer'] = observer.snapshot()
    assert r['observer']['sampler']['selected_sampler']=='RX_A'
    assert r['observer']['interlock']['state']=='READY'
finally:
    observer.close()
start = time.monotonic(); capture = c.api('capture'); r['capture_seconds'] = time.monotonic()-start
raw = base64.b64decode(capture.pop('iq_f32le'),validate=True)
assert len(raw)==131072 and hashlib.sha256(raw).hexdigest()==capture['iq_sha256'] and not capture['body']['keyed']
(p/'batch-receive.f32le').write_bytes(raw); r['capture'] = capture
guard = c.api('guard-begin')['body']; r['guard_begin'] = guard
assert guard['state']=='observing' and guard['remaining_ms']>7000
r['guard_status'] = c.api('guard-status',guard['test_id'])['body']
r['guard_stop'] = c.api('guard-stop',guard['test_id'])['body']
c.wait(lambda:c.api('guard-status',guard['test_id'])['body']['state']=='ended')
r['guard_ended'] = c.api('guard-status',guard['test_id'])['body']
assert not c.bridge({'cmd':'get','model':'radio'})['radio']['transmitting']
r['status'] = c.api('status')['body']; assert r['status']['registered'] and not r['status']['keyed'] and not r['status']['correction_enabled']
r['passed'] = True
out.write_text(json.dumps(r,indent=2)+'\n')
print(json.dumps({'prepared':True,'capture_seconds':r['capture_seconds'],'RF':False,'guard_ended':True}))
