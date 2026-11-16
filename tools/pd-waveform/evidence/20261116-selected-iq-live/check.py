import base64,hashlib,json,math,pathlib,struct,subprocess,sys,time
sys.path.insert(0,'/home/jeremy/AetherSDR/build/aetherpd-source/tools')
from automation_probe import Bridge
out=pathlib.Path(__file__).resolve().parent
vault=out.parent/'first-iq-live/vault-v3'
b=Bridge('/tmp/aetherpd-first-iq-fresh')
report={'passed':False,'rf_commands':[],'cases':[]}; active=False

def call(cmd):
 r=b.request(cmd,5);assert r.get('ok'),r;return r

def invoke(target,action='click',value=None):
 q={'cmd':'invoke','target':target,'action':action}
 if value is not None:q['value']=value
 return call(q)

def req(path):
 r=subprocess.run([str(vault),path],capture_output=True,text=True,timeout=22)
 assert r.stdout.strip().startswith('{'),r.stderr[:500]
 return json.loads(r.stdout)

def tree():
 r=call({'cmd':'dumpTree'});found={}
 def walk(x):
  if isinstance(x,dict):
   if str(x.get('objectName','')).startswith('pd'):found[x['objectName']]={k:v for k,v in x.items() if k!='children'}
   for v in x.values():walk(v)
  elif isinstance(x,list):
   for v in x:walk(v)
 walk(r);return found

def wait_for(fn,seconds=20):
 deadline=time.monotonic()+seconds
 while True:
  value=fn()
  if value:return value
  if time.monotonic()>deadline:raise TimeoutError('condition not reached')
  time.sleep(.25)

def safe():
 r=call({'cmd':'get','model':'radio'})['radio']
 assert r['connected'] and not r['transmitting'] and not r['fullDuplex'],r
 assert not call({'cmd':'whoami'})['txAllowed']
 return r

def status_when(predicate):
 r=req('aetherpd/status')
 return r if r['http_status']==200 and predicate(r['body']) else None

def stop():
 global active
 invoke('pdStop');r=wait_for(lambda:status_when(lambda x:x.get('state')=='stopped'),30)
 active=False;return r

try:
 report['before']=safe();pan=call({'cmd':'get','model':'pans'})['pans'];assert len(pan)==1
 pan=pan[0];assert pan['ownedByUs'] and pan['rxAntenna']=='RX_A' and pan['rfGain']==0 and abs(pan['centerMhz']-7.15)<1e-6
 for channel in (2,3,4):
  case={'channel':channel,'passed':False};report['cases'].append(case)
  invoke('daxPanel/QComboBox','setCurrentIndex',channel)
  def selected_choice():
   t=tree();c=t['pdIqSelection']
   return t if len(c['items'])==2 and c['items'][1].startswith('IQ Ch '+str(channel)+' ') else None
  case['selection_before']=wait_for(selected_choice)
  invoke('pdIqSelection','setCurrentIndex',1)
  wait_for(lambda:tree()['pdStart']['enabled'])
  case['start']=invoke('pdStart');active=True
  case['registered']=wait_for(lambda:status_when(lambda x:x.get('registered')),35)
  assert not case['registered']['body']['keyed'] and not case['registered']['body']['correcting']
  def capture_ready():
   r=req('aetherpd/capture');return r if r['http_status']==200 else None
  case['capture']=wait_for(capture_ready)
  c=case['capture']['body'];binding=c['feedback_binding']
  assert binding=={'channel':channel,'failed':False,'gui_handle':int(pan['clientHandle'],16),'pan':int(pan['panId'],16),'transport':'local_dax_iq','valid':True},binding
  assert not c['keyed'] and not c['correcting'] and c['sample_rate_hz']==48000 and c['sample_count']==16384
  raw=bytearray()
  for chunk in range(32):
   r=req('aetherpd/capture/'+c['capture_id']+'/'+str(chunk));assert r['http_status']==200,r
   part=r['body'];assert part['offset']==chunk*512
   data=base64.b64decode(part['iq_f32le'],validate=True);assert len(data)==4096;raw.extend(data)
  values=struct.unpack('<'+str(len(raw)//4)+'f',raw)
  assert all(math.isfinite(v) for v in values) and any(v!=0 for v in values)
  path=out/('channel-'+str(channel)+'.f32le');path.write_bytes(raw)
  case.update(iq_bytes=len(raw),iq_sha256=hashlib.sha256(raw).hexdigest(),nonzero_values=sum(v!=0 for v in values))
  case['running_controls']=tree();assert not case['running_controls']['pdIqSelection']['enabled']
  case['blocked_running_selection']=b.request({'cmd':'invoke','target':'pdIqSelection','action':'setCurrentIndex','value':0},5)
  assert not case['blocked_running_selection']['ok']
  case['after']=safe()
  if channel!=4:case['stop']=stop()
  case['passed']=True
  (out/'result.json').write_text(json.dumps(report,indent=2))
  print(json.dumps({'channel':channel,'capture_passed':True,'samples':16384}),flush=True)
 # Mutate the selected radio channel during a running receive-only session.
 loss={'passed':False,'old_channel':4,'new_channel':1};report['binding_loss']=loss
 loss['change']=invoke('daxPanel/QComboBox','setCurrentIndex',1);start=time.monotonic();loss['observations']=[]
 for _ in range(50):
  r=req('aetherpd/status');loss['observations'].append({'elapsed_s':round(time.monotonic()-start,3),'status':r})
  body=r.get('body',{});binding=body.get('feedback_binding',{})
  if body.get('state') in ('stopped','stopped_receive_unconfirmed') or binding.get('failed') or not body.get('available',True):break
  time.sleep(.2)
 loss['panel_after_loss']=tree();loss['capture_after_loss']=req('aetherpd/capture')
 assert loss['capture_after_loss']['http_status']!=200,loss['capture_after_loss']
 # Returning the radio channel must not revive the failed session.
 loss['return_channel']=invoke('daxPanel/QComboBox','setCurrentIndex',4)
 time.sleep(2)
 loss['status_after_return']=req('aetherpd/status');loss['capture_after_return']=req('aetherpd/capture')
 assert loss['capture_after_return']['http_status']!=200,loss['capture_after_return']
 loss['after']=safe();loss['stop']=stop();loss['passed']=True
 report['after']=safe();report['passed']=True
except Exception as e:
 report['error']=type(e).__name__+': '+str(e)
finally:
 if active:
  try:report['final_stop']=stop()
  except Exception as e:report['stop_error']=str(e);report['passed']=False
 (out/'result.json').write_text(json.dumps(report,indent=2))
 print(json.dumps({'passed':report['passed'],'error':report.get('error'),'stop_error':report.get('stop_error')}),flush=True)
 if not report['passed']:raise SystemExit(1)
