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
 report['before']=safe();prior=json.loads((out/'owner-disconnect.json').read_text())
 report['pans']=wait_for(lambda:call({'cmd':'get','model':'pans'})['pans'])
 assert report['pans'][0]['clientHandle']!=prior['before_pans']['pans'][0]['clientHandle']
 report['retired_controls']=tree();assert not report['retired_controls']['pdStart']['enabled'] and not report['retired_controls']['pdArm']['enabled']
 report['before_capture']=req('aetherpd/capture');assert report['before_capture']['http_status']!=200
 invoke('pdDone');time.sleep(.2);invoke('openAetherPD')
 report['fresh_controls']=wait_for(lambda:tree() if 'pdIqSelection' in tree() else None)
 wait_for(lambda:tree()['pdIqSelection']['enabled'])
 report['fresh_controls']=tree();assert report['fresh_controls']['pdIqSelection']['currentIndex']==0
 assert not report['fresh_controls']['pdStart']['enabled']
 assert len(report['fresh_controls']['pdIqSelection']['items'])==2
 invoke('pdIqSelection','setCurrentIndex',1);wait_for(lambda:tree()['pdStart']['enabled'])
 report['start']=invoke('pdStart');active=True
 report['registered']=wait_for(lambda:status_when(lambda x:x.get('registered')),35)
 def captured():
  r=req('aetherpd/capture');return r if r['http_status']==200 else None
 report['capture']=wait_for(captured)
 binding=report['capture']['body']['feedback_binding'];assert binding['valid'] and not binding['failed']
 assert binding['gui_handle']==int(report['pans'][0]['clientHandle'],16)
 assert binding['gui_handle']!=prior['registered']['body']['feedback_binding']['gui_handle']
 report['stop']=stop();report['after']=safe();report['passed']=True
except Exception as e:report['error']=type(e).__name__+': '+str(e)
finally:
 if active:
  try:report['final_stop']=stop()
  except Exception as e:report['stop_error']=str(e);report['passed']=False
 (out/'reconnect-recovery.json').write_text(json.dumps(report,indent=2))
 print(json.dumps({'passed':report['passed'],'error':report.get('error'),'stop_error':report.get('stop_error')}),flush=True)
 if not report['passed']:raise SystemExit(1)
