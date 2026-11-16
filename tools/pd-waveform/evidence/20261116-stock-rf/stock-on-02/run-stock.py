"""One explicit GUI-keyed seven-second stock comparison; no automatic RF retry."""
import base64
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time
import traceback

p = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('common',p/'bench-common.py')
c = importlib.util.module_from_spec(spec); spec.loader.exec_module(c)
from bench_audio_route_native import Route, SINK
import numpy as np

label = sys.argv[1]; assert label in ('stock-off-01','stock-on-01','stock-off-02','stock-on-02')
enabled = '-on-' in label
pc_gain = 80 if label.endswith('-02') else 35
out = p/label; out.mkdir(exist_ok=False)
r = {'case':label,'stock_apd_requested':enabled,'reset_stock_model':False,'key_intents':[], 'rows':[], 'captures':[], 'cleanup_errors':[]}
route = None; player = None; capture = None; capture_file = None; observer = None; guard_id = None; key_attempted = False
def save():
    target = out/'result.json'; temp = out/'result.json.new'
    temp.write_text(json.dumps(r,indent=2)+'\n'); temp.replace(target)

def state():
    return {m:c.bridge({'cmd':'get','model':m})[m] for m in ('radio','transmit','slices','pans')}

try:
    for previous in p.glob('stock-*/result.json'):
        old = json.loads(previous.read_text())
        assert old.get('finished_monotonic_ns') and time.monotonic_ns()-old['finished_monotonic_ns'] >= 31_000_000_000, 'Cooldown or unresolved prior attempt'
    r['identity'] = c.bridge({'cmd':'whoami'}); assert r['identity']['txAllowed'] and r['identity']['pid']==1365774
    assert hashlib.sha256(Path('/proc/1365774/exe').read_bytes()).hexdigest()=='e6813ac61cff90ae55eb7946958fee0613cb50f4e4e32ff744dfcce30ba29128'
    r['before'] = state(); s = r['before']; tx=s['transmit']; sl=s['slices']; pan=s['pans']
    assert s['radio']['connected'] and s['radio']['serial']=='2125-1213-8600-8895' and not s['radio']['transmitting'] and not s['radio']['fullDuplex']
    assert tx['rfPower']==10 and tx['maxPowerLevel']==10 and tx['micSelection']=='PC' and not tx['voxEnable'] and not tx['speechProc'] and not tx['dax']
    assert len(sl)==1 and sl[0]['mode']=='USB' and sl[0]['frequency']==7.15 and sl[0]['txAntenna']=='ANT1' and sl[0]['rxAntenna']=='RX_A' and sl[0]['txSlice'] and not sl[0]['xitOn']
    assert len(pan)==1 and pan[0]['rfGain']==0 and pan[0]['centerMhz']==7.15
    native=c.api('status')['body']; assert native['registered'] and not native['keyed'] and not native['correction_enabled']
    c.invoke('APD pre-distortion','setChecked','true' if enabled else 'false')
    c.wait(lambda:c.bridge({'cmd':'get','model':'transmit'})['transmit']['apdEnabled']==enabled)
    observer=c.Observer(); observed=observer.snapshot(); r['observer_before']=observed
    assert observed['sampler'].get('selected_sampler')=='RX_A' and observed['apd'].get('enable')==str(int(enabled))
    assert all(observed['interlock'].get(k)==v for k,v in {'state':'READY','source':'','reason':'','tx_client_handle':'0x00000000','tx_allowed':'1'}.items())
    rate=48000; samples=np.arange(8*rate)/rate
    tone=.415*(np.cos(2*np.pi*710*samples)+np.cos(2*np.pi*1900*samples))
    pcm=np.repeat(tone[:,None],2,axis=1).astype('<f4').tobytes(); (out/'source.f32le').write_bytes(pcm)
    r['source']={'sample_rate':rate,'channels':2,'seconds':8,'tone_hz':[710,1900],'each_peak':.415,'PC_gain':pc_gain,'sha256':hashlib.sha256(pcm).hexdigest()}
    c.invoke('Microphone gain','setValue',str(pc_gain))
    route=Route(out/'route-cleanup.json'); r['route']=route.start(); route.verify()
    player_log=(out/'player.log').open('w')
    player=subprocess.Popen(['pacat','--playback','--device='+SINK,'--rate=48000','--channels=2','--format=float32le','--latency-msec=20','--client-name=AetherPD-Stock-Bench',str(out/'source.f32le')],stdout=subprocess.DEVNULL,stderr=player_log)
    player_log.close(); time.sleep(.15); assert player.poll() is None
    guard=c.api('guard-begin')['body']; guard_id=guard['test_id']; r['guard_begin']=guard
    assert guard['state']=='observing' and guard['remaining_ms']>8000
    route.verify(); assert not c.bridge({'cmd':'get','model':'radio'})['radio']['transmitting']
    r['key_intents'].append({'monotonic_ns':time.monotonic_ns(),'frequency_mhz':7.15,'nominal_watts':10,'GUI_key_seconds':7,'source_seconds':8,'native_cleanup_after_ms':9000})
    save(); key_attempted=True; start=time.monotonic()
    r['key_response']=c.bridge({'cmd':'key','action':'ptt','value':'on'})
    saw_key=False
    while time.monotonic()-start<7:
        elapsed=time.monotonic()-start
        radio=c.bridge({'cmd':'get','model':'radio'})['radio']
        tx=c.bridge({'cmd':'get','model':'transmit'})['transmit']
        meters=c.bridge({'cmd':'get','model':'meters'})['meters']
        observed=observer.snapshot()
        row={'seconds':elapsed,'radio':radio,'transmit':tx,'meters':meters,**observed};r['rows'].append(row)
        assert radio['connected'] and not radio['fullDuplex'] and tx['rfPower']==10 and tx['maxPowerLevel']==10 and tx['apdEnabled']==enabled
        assert observed['sampler'].get('selected_sampler')=='RX_A'
        saw_key |= radio['transmitting']
        if saw_key and not radio['transmitting']:
            raise RuntimeError('Radio released before seven-second capture finished')
        fwd=meters.get('fwdPowerInstant'); reflected=meters.get('reflectedPower'); swr=meters.get('swr')
        if isinstance(fwd,(int,float)): assert math.isfinite(fwd) and fwd<=11, ('forward power stop',fwd)
        if isinstance(reflected,(int,float)): assert math.isfinite(reflected) and reflected<=.1, ('reflected power stop',reflected)
        if elapsed>1:
            assert saw_key and isinstance(fwd,(int,float)) and fwd>=0
            assert 0<=meters.get('fwdPowerAgeMs',99999)<=250 and 0<=meters.get('reflectedPowerAgeMs',99999)<=250
            if isinstance(swr,(int,float)) and swr>0: assert swr<2, ('SWR stop',swr)
        assert player.poll() is None, 'Finite audio source ended before requested stop'
        route.verify()
        if capture is not None and capture.poll() is not None:
            capture_file.close(); result=json.loads((out/f'capture-{len(r["captures"])}.json').read_text()); assert capture.returncode==0 and result['complete'],result
            manifest=result['body']; assert manifest['keyed'] and not manifest['correcting']
            binding=manifest['feedback_binding']; assert binding['valid'] and binding['channel']==4 and binding['gui_handle']==int(pan[0]['clientHandle'],16)
            raw=base64.b64decode(result['iq_f32le'],validate=True); assert len(raw)==131072 and hashlib.sha256(raw).hexdigest()==result['iq_sha256']
            assert all(math.isfinite(v) for v in struct.unpack('<32768f',raw))
            r['captures'].append({'manifest':manifest,'sha256':result['iq_sha256'],'completed_s':elapsed,'stock_apd':dict(observer.apd)})
            (out/f'frame-{len(r["captures"])-1}.f32le').write_bytes(raw); capture=None
        if elapsed>=3 and capture is None and len(r['captures'])<4:
            capture_file=(out/f'capture-{len(r["captures"])}.json').open('w')
            capture=subprocess.Popen([str(p/'bench-api'),'capture'],stdout=capture_file,stderr=subprocess.DEVNULL)
        time.sleep(.04)
    r['GUI_stop']=c.bridge({'cmd':'key','action':'ptt','value':'off'}); r['GUI_stop_s']=time.monotonic()-start
    assert len(r['captures'])==4, 'Four complete captures required'
    assert len({f['manifest']['capture_id'] for f in r['captures']})==4
    assert len({f['manifest']['source']['generation'] for f in r['captures']})==1, 'Capture generation changed'
    if enabled:
        assert all(f['stock_apd'].get('equalizer_active')=='1' for f in r['captures']), 'Stock equalizer activity not confirmed for all frames'
    raw=b''.join((out/f'frame-{i}.f32le').read_bytes() for i in range(4)); (out/'four-frames.f32le').write_bytes(raw)
    result=subprocess.run([str(p/'analyze-raw'),str(out/'four-frames.f32le')],capture_output=True,text=True,check=True)
    r['analysis']=json.loads(result.stdout); r['completed']=True
except BaseException as error:
    r['error']=repr(error); r['traceback']=traceback.format_exc()
finally:
    if key_attempted:
        try:r['finally_stop']=c.bridge({'cmd':'key','action':'ptt','value':'off'})
        except BaseException as error:r['cleanup_errors'].append('GUI stop: '+repr(error))
    if guard_id:
        try:
            r['guard_stop']=c.api('guard-stop',guard_id)['body']
            c.wait(lambda:c.api('guard-status',guard_id)['body']['state']=='ended')
            r['guard_final']=c.api('guard-status',guard_id)['body']
        except BaseException as error:r['cleanup_errors'].append('Guard: '+repr(error))
    if capture:
        try:capture.wait(timeout=16)
        except subprocess.TimeoutExpired:capture.terminate();capture.wait(timeout=2)
    if capture_file: capture_file.close()
    if player:
        if player.poll() is None:player.terminate()
        player.wait(timeout=3)
    if route:
        try:
            r['route_cleanup']=route.close();assert not r['route_cleanup']['errors'] and r['route_cleanup']['module_removed']
        except BaseException as error:r['cleanup_errors'].append('Route: '+repr(error))
    if observer:
        r['observer_after']=observer.snapshot();r['observer_rows']=observer.rows;observer.close()
    try:
        c.wait(lambda:not c.bridge({'cmd':'get','model':'radio'})['radio']['transmitting'])
        r['after']=state();r['native_after']=c.api('status')['body']
        assert not r['native_after']['keyed'] and not r['native_after']['correction_enabled']
        r['idle_verified']=True
    except BaseException as error:r['cleanup_errors'].append('Idle check: '+repr(error))
    r['finished_monotonic_ns']=time.monotonic_ns();save()
assert r.get('completed') and r.get('idle_verified') and not r['cleanup_errors'], r.get('error',r['cleanup_errors'])
print(json.dumps({'case':label,'analysis':r['analysis'],'GUI_stop_s':r['GUI_stop_s'],'idle':True}))
