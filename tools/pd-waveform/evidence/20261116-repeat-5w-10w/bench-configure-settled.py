"""Explicit receive-only configuration for a named matched RF capture."""
import json,pathlib,subprocess,sys,time
p=pathlib.Path(__file__).resolve().parent;power=int(sys.argv[1]);mode=sys.argv[2];assert power in (5,10) and mode in ('off','on')
case=sys.argv[3];assert case in ('repeat','tenw');output=p/(f'configure-{case}-{power}-{mode}.json');assert not output.exists()
sys.path.insert(0,str(p.parents[1]/'aetherpd-source/tools'));from automation_probe import Bridge
b=Bridge('/tmp/aetherpd-fit-worker');r={'power':power,'correction':mode,'commands':[]}
def req(q):
 x=b.request(q,5);assert x['ok'],x;r['commands'].append({'request':q,'response':x});return x
def click(name):
 end=time.monotonic()+10
 while True:
  items=[]
  def walk(x):
   if isinstance(x,dict):
    if x.get('objectName')==name:items.append(x)
    for v in x.values():walk(v)
   elif isinstance(x,list):
    for v in x:walk(v)
  walk(req({'cmd':'dumpTree'}))
  if len(items)==1 and items[0].get('enabled'):break
  assert time.monotonic()<end,('Control did not settle',name);time.sleep(.15)
 return req({'cmd':'invoke','target':name,'action':'click'})
def status():
 x=subprocess.run([str(p.parent/'first-iq-live/vault-v3'),'aetherpd/status'],capture_output=True,text=True,timeout=15);s=json.loads(x.stdout);assert s['http_status']==200;return s['body']
r['before']=req({'cmd':'get','model':'radio'})['radio'];assert not r['before']['transmitting'] and not r['before']['fullDuplex']
s=status();assert s['registered'] and not s['keyed']
req({'cmd':'invoke','target':'pdMeasureRadioPower','action':'setValue','value':power})
if req({'cmd':'get','model':'transmit'})['transmit']['rfPower']!=power:click('pdMeasureApplyPower')
end=time.monotonic()+8
while req({'cmd':'get','model':'transmit'})['transmit']['rfPower']!=power:
 assert time.monotonic()<end;time.sleep(.2)
s=status()
if s['correction_enabled']!=(mode=='on'):click('pdEnableCorrection' if mode=='on' else 'pdBypassCorrection')
end=time.monotonic()+8
while True:
 s=status()
 if s['correction_enabled']==(mode=='on'):break
 assert time.monotonic()<end;time.sleep(.2)
assert s['registered'] and not s['keyed'] and s['feedback_binding']['valid'] and s['feedback_binding']['channel']==4
r['status']=s;r['transmit']=req({'cmd':'get','model':'transmit'})['transmit'];assert r['transmit']['rfPower']==power and r['transmit']['maxPowerLevel']<=10 and not r['transmit']['apdEnabled']
output.write_text(json.dumps(r,indent=2));print(json.dumps({'watts':power,'correction':mode,'RF':False}))
