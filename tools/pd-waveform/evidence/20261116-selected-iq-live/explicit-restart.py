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
 report['before']=safe();report['prior_status']=req('aetherpd/status')
 assert report['prior_status']['body']=={'available':False,'state':'cleanup_unconfirmed'}
 invoke('pdRefresh');time.sleep(.5);report['before_controls']=tree()
 assert not report['before_controls']['pdArm']['enabled']
 assert report['before_controls']['pdIqSelection']['items'][1].startswith('IQ Ch 4 ')
 invoke('pdIqSelection','setCurrentIndex',1);wait_for(lambda:tree()['pdStart']['enabled'])
 report['start']=invoke('pdStart');active=True;started=time.monotonic()
 report['registered']=wait_for(lambda:status_when(lambda x:x.get('registered')),35)
 report['ready_s']=round(time.monotonic()-started,3)
 def captured():
  r=req('aetherpd/capture');return r if r['http_status']==200 else None
 report['capture']=wait_for(captured)
 binding=report['capture']['body']['feedback_binding']
 assert binding['channel']==4 and binding['valid'] and not binding['failed']
 assert not report['capture']['body']['keyed'] and not report['capture']['body']['correcting']
 report['stop']=stop();report['after']=safe();report['passed']=True
except Exception as e:report['error']=type(e).__name__+': '+str(e)
finally:
 if active:
  try:report['final_stop']=stop()
  except Exception as e:report['stop_error']=str(e);report['passed']=False
 (out/'explicit-restart.json').write_text(json.dumps(report,indent=2))
 print(json.dumps({'passed':report['passed'],'ready_s':report.get('ready_s'),'error':report.get('error'),'stop_error':report.get('stop_error')}),flush=True)
 if not report['passed']:raise SystemExit(1)
