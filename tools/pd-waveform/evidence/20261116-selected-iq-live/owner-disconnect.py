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
 report['before']=safe();report['before_pans']=call({'cmd':'get','model':'pans'})
 invoke('pdIqSelection','setCurrentIndex',1);wait_for(lambda:tree()['pdStart']['enabled'])
 report['start']=invoke('pdStart');active=True
 report['registered']=wait_for(lambda:status_when(lambda x:x.get('registered')),35)
 binding=report['registered']['body']['feedback_binding'];assert binding['valid'] and binding['channel']==4
 report['disconnect']=call({'cmd':'disconnect'});started=time.monotonic()
 report['disconnected']=wait_for(lambda:not call({'cmd':'get','model':'radio'})['radio']['connected'])
 report['terminal']=wait_for(lambda:status_when(lambda x:x.get('state') in ('stopped','cleanup_unconfirmed','stopped_receive_unconfirmed')),30)
 report['terminal_s']=round(time.monotonic()-started,3);active=False
 report['capture_after_disconnect']=req('aetherpd/capture');assert report['capture_after_disconnect']['http_status']!=200
 report['controls_after_disconnect']=tree()
 assert not report['controls_after_disconnect']['pdStart']['enabled'] and not report['controls_after_disconnect']['pdArm']['enabled']
 report['old_window_start_refused']=b.request({'cmd':'invoke','target':'pdStart','action':'click'},5);assert not report['old_window_start_refused']['ok']
 report['connect']=call({'cmd':'connect','action':'local','value':'serial 2125-1213-8600-8895'})
 wait_for(lambda:call({'cmd':'get','model':'radio'})['radio']['connected'],30)
 report['after']=safe();report['after_pans']=call({'cmd':'get','model':'pans'})
 assert report['after_pans']['pans'][0]['clientHandle']!=report['before_pans']['pans'][0]['clientHandle']
 report['capture_after_reconnect']=req('aetherpd/capture');assert report['capture_after_reconnect']['http_status']!=200
 report['controls_after_reconnect']=tree()
 assert not report['controls_after_reconnect']['pdStart']['enabled'] and not report['controls_after_reconnect']['pdArm']['enabled']
 report['passed']=True
except Exception as e:report['error']=type(e).__name__+': '+str(e)
finally:
 if active:
  try:report['final_stop']=stop()
  except Exception as e:report['stop_error']=str(e);report['passed']=False
 (out/'owner-disconnect.json').write_text(json.dumps(report,indent=2))
 print(json.dumps({'passed':report['passed'],'terminal_s':report.get('terminal_s'),'error':report.get('error'),'stop_error':report.get('stop_error')}),flush=True)
 if not report['passed']:raise SystemExit(1)
