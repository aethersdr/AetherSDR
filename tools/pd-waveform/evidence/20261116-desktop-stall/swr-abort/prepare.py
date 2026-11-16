import json,pathlib,sys,time
sys.path.insert(0,'/home/jeremy/AetherSDR/build/aetherpd-source/tools');from automation_probe import Bridge
p=pathlib.Path(__file__).resolve().parent;b=Bridge('/tmp/aetherpd-usb-start');r={}
def req(q):
 x=b.request(q,8);assert x['ok'],x;return x
def invoke(n,a='click',v=None):
 q={'cmd':'invoke','target':n,'action':a}
 if v is not None:q['value']=v
 return req(q)
def widgets():
 out={}
 def walk(x):
  if isinstance(x,dict):
   if x.get('objectName','').startswith('pd'):out[x['objectName']]={k:v for k,v in x.items() if k!='children'}
   for v in x.values():walk(v)
  elif isinstance(x,list):
   for v in x:walk(v)
 walk(req({'cmd':'dumpTree'}));return out
def wait(f):
 end=time.monotonic()+25
 while True:
  x=f()
  if x:return x
  assert time.monotonic()<end,'Preparation timeout';time.sleep(.2)
r['identity']=req({'cmd':'whoami'});assert r['identity']['txAllowed']
if not req({'cmd':'get','model':'radio'})['radio']['connected']:req({'cmd':'connect','action':'local','value':'serial 2125-1213-8600-8895'})
wait(lambda:req({'cmd':'get','model':'radio'})['radio']['connected'])
wait(lambda:widgets().get('pdIqSelection',{}).get('enabled'));invoke('pdIqSelection','setCurrentIndex',1);wait(lambda:widgets().get('pdStart',{}).get('enabled'));invoke('pdStart');wait(lambda:widgets().get('pdMeasurement',{}).get('enabled'))
wait(lambda:'binding confirmed' in widgets().get('pdIqObserved',{}).get('value',''))
req({'cmd':'slice','action':'mode','value':'PD','id':'0'});wait(lambda:req({'cmd':'get','model':'slices'})['slices'][0]['mode']=='PD')
invoke('pdMeasurement');wait(lambda:'pdMeasureRadioPower' in widgets());invoke('pdMeasureRadioPower','setValue',5)
if req({'cmd':'get','model':'transmit'})['transmit']['rfPower']!=5:
 wait(lambda:widgets()['pdMeasureApplyPower']['enabled']);invoke('pdMeasureApplyPower');wait(lambda:req({'cmd':'get','model':'transmit'})['transmit']['rfPower']==5)
invoke('pdMeasureAverages','setValue',4);invoke('pdMeasureTimeout','setValue',10)
for m in ['radio','transmit','slices','pans']:r[m]=req({'cmd':'get','model':m})
assert not r['radio']['radio']['transmitting'] and not r['radio']['radio']['fullDuplex'];assert r['transmit']['transmit']['rfPower']==5 and r['transmit']['transmit']['maxPowerLevel']<=10
r['controls']=widgets();(p/'prepared.json').write_text(json.dumps(r,indent=2));print('Prepared: 5 W, timeout 10 s, local IQ channel4, FDX off; no RF yet.')
