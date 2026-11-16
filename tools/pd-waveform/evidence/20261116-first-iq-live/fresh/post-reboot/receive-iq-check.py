import base64,hashlib,json,math,pathlib,struct,subprocess,sys,time
sys.path.insert(0,'/home/jeremy/AetherSDR/build/aetherpd-source/tools');from automation_probe import Bridge
out=pathlib.Path(__file__).resolve().parent;b=Bridge('/tmp/aetherpd-first-iq-fresh');report={'passed':False,'rf_commands':[],'start_requested':False}
def req(path):
 p=subprocess.run([str(out.parents[1]/'vault-v3'),path],capture_output=True,text=True,timeout=22)
 assert p.stdout.strip().startswith('{'),'status helper failed'
 return json.loads(p.stdout)
def radio():return b.request({'cmd':'get','model':'radio'},5)['radio']
try:
 report['before']=radio();assert report['before']['connected'] and not report['before']['transmitting'] and not report['before']['fullDuplex']
 assert not b.request({'cmd':'whoami'},5)['txAllowed']
 pans=b.request({'cmd':'get','model':'pans'},5)['pans'];report['pans_before']=pans
 assert len(pans)==1 and pans[0]['ownedByUs'] and pans[0]['rxAntenna']=='RX_A' and pans[0]['rfGain']==0 and abs(pans[0]['centerMhz']-7.15)<1e-6
 choice=b.request({'cmd':'invoke','target':'pdIqSelection','action':'setCurrentIndex','value':1},5);assert choice['ok'];report['choice']=choice
 report['start']=b.request({'cmd':'invoke','target':'pdStart','action':'click'},5);assert report['start']['ok'];report['start_requested']=True
 deadline=time.monotonic()+35
 while time.monotonic()<deadline:
  result=req('aetherpd/status')
  if result['http_status']==200 and result['body'].get('registered'):
   report['registered_status']=result['body'];assert not result['body']['keyed'] and not result['body']['correcting'];break
  time.sleep(.5)
 assert 'registered_status' in report,result
 deadline=time.monotonic()+15
 while time.monotonic()<deadline:
  result=req('aetherpd/capture')
  if result['http_status']==200:break
  time.sleep(.5)
 assert result['http_status']==200,result
 capture=result['body'];report['capture']=capture
 binding=capture['feedback_binding'];assert binding['transport']=='local_dax_iq' and binding['valid'] and not binding['failed'] and binding['channel']==1
 assert binding['pan']==int(pans[0]['panId'],16) and binding['gui_handle']==int(pans[0]['clientHandle'],16)
 assert not capture['keyed'] and not capture['correcting'] and capture['sample_rate_hz']==48000 and capture['sample_count']==16384
 raw=bytearray()
 for chunk in range(32):
  result=req('aetherpd/capture/'+capture['capture_id']+'/'+str(chunk));assert result['http_status']==200,result
  part=result['body'];assert part['offset']==chunk*512
  data=base64.b64decode(part['iq_f32le'],validate=True);assert len(data)==4096;raw.extend(data)
 values=struct.unpack('<'+str(len(raw)//4)+'f',raw);assert all(math.isfinite(v) for v in values) and any(v!=0 for v in values)
 (out/'receive-iq.f32le').write_bytes(raw);report.update(iq_bytes=len(raw),iq_sha256=hashlib.sha256(raw).hexdigest(),rms=math.sqrt(sum(v*v for v in values)/len(values)),nonzero_values=sum(v!=0 for v in values))
 report['final_status']=req('aetherpd/status');assert report['final_status']['http_status']==200 and not report['final_status']['body']['keyed'] and not report['final_status']['body']['correcting']
 report['after']=radio();assert report['after']['connected'] and not report['after']['transmitting'] and not report['after']['fullDuplex'];report['passed']=True
except Exception as e:report['error']=type(e).__name__+': '+str(e)
finally:
 if report['start_requested']:
  try:
   report['stop']=b.request({'cmd':'invoke','target':'pdStop','action':'click'},5);assert report['stop']['ok']
  except Exception as e:report['stop_error']=str(e);report['passed']=False
 (out/'receive-iq-check.json').write_text(json.dumps(report,indent=2));print(json.dumps({k:report[k] for k in ['passed','error','iq_bytes','rms','nonzero_values','stop_error'] if k in report}),flush=True)
 if not report['passed']:raise SystemExit(1)
