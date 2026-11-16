import base64,json,math,pathlib,struct,subprocess,sys,time,hashlib
p=pathlib.Path(__file__).resolve().parent;b=p.parent;sys.path.insert(0,str(b.parent/'aetherpd-source/tools'));from automation_probe import Bridge
bridge=Bridge('/tmp/aetherpd-usb-start');r={'passed':False,'RF_commands':[]}
def req(path):
 x=subprocess.run([str(b/'first-iq-live/vault-v3'),path],capture_output=True,text=True,timeout=15);return json.loads(x.stdout)
try:
 r['radio_before']=bridge.request({'cmd':'get','model':'radio'},5);assert not r['radio_before']['radio']['transmitting'] and not r['radio_before']['radio']['fullDuplex']
 r['status_before']=req('aetherpd/status');s=r['status_before']['body'];assert s['registered'] and s['calibration_required'] and not s['keyed'] and not s['correcting'] and s['calibration_state']=='idle' and s['faults']['stale_iq']>0
 r['capture']=req('aetherpd/capture');assert r['capture']['http_status']==200;c=r['capture']['body'];before=json.loads((p/'before-stall.json').read_text())['capture']['body'];assert c['capture_id']!=before['capture_id'] and not c['keyed'] and not c['correcting'] and c['sample_count']==16384 and c['sample_rate_hz']==48000 and c['feedback_binding']['valid']
 data=bytearray()
 for chunk in range(32):
  part=req('aetherpd/capture/'+c['capture_id']+'/'+str(chunk));assert part['http_status']==200;part=part['body'];assert part['offset']==chunk*512;raw=base64.b64decode(part['iq_f32le'],validate=True);assert len(raw)==4096;data.extend(raw)
 samples=struct.unpack('<32768f',data);assert all(math.isfinite(x) for x in samples) and any(x!=0 for x in samples);(p/'fresh-after-stall.f32le').write_bytes(data);r['IQ_bytes']=len(data);r['IQ_sha256']=hashlib.sha256(data).hexdigest();r['passed']=True
except Exception as e:r['error']=type(e).__name__+': '+str(e)
finally:
 r['stop']=bridge.request({'cmd':'invoke','target':'pdStop','action':'click'},5);assert r['stop']['ok'];end=time.monotonic()+20
 while True:
  r['stopped']=req('aetherpd/status')
  if r['stopped'].get('body',{}).get('state')=='stopped':break
  assert time.monotonic()<end;time.sleep(.2)
 r['radio_after']=bridge.request({'cmd':'get','model':'radio'},5);assert not r['radio_after']['radio']['transmitting'];(p/'capture-after-stall.json').write_text(json.dumps(r,indent=2)+'\n');print({k:r[k] for k in ['passed','error','IQ_bytes'] if k in r});assert r['passed']
