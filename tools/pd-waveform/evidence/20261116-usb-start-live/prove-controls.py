import sys,pathlib,json,time
p=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(p.parents[1]/'aetherpd-source/tools'));from automation_probe import Bridge
end=time.monotonic()+25
while not pathlib.Path('/tmp/aetherpd-usb-start').exists():
 assert time.monotonic()<end;time.sleep(.2)
b=Bridge('/tmp/aetherpd-usb-start');record={'RF_commands':[]}
def req(q):
 r=b.request(q,7);assert r.get('ok'),r;return r
def radio():return req({'cmd':'get','model':'radio'})['radio']
def wait(f):
 end=time.monotonic()+25
 while True:
  value=f()
  if value:return value
  assert time.monotonic()<end;time.sleep(.15)
def panel():
 out={}
 def walk(x):
  if isinstance(x,dict):
   if x.get('objectName','').startswith('pd'):out[x['objectName']]={k:x[k] for k in ['value','enabled','toolTip','accessibleDescription'] if k in x}
   for v in x.values():walk(v)
  elif isinstance(x,list):
   for v in x:walk(v)
 walk(req({'cmd':'dumpTree','target':'AetherPDDialog'}));return out
def slices():return req({'cmd':'get','model':'slices'})['slices']
assert req({'cmd':'whoami'})['pid']==json.loads((p/'instance.json').read_text())['pid']
if not radio()['connected']:req({'cmd':'connect','action':'local','value':'serial 2125-1213-8600-8895'})
wait(lambda:radio()['connected']);assert not radio()['transmitting']
req({'cmd':'invoke','target':'Waveforms...','action':'trigger'});req({'cmd':'invoke','target':'openAetherPD','action':'click'})
wait(lambda:panel().get('pdIqSelection',{}).get('enabled'))
req({'cmd':'invoke','target':'pdIqSelection','action':'setCurrentIndex','value':1})
req({'cmd':'slice','action':'mode','value':'DIGU','id':'0'});wait(lambda:slices()[0]['mode']=='DIGU')
record['DIGU']=wait(lambda:panel() if not panel().get('pdStart',{}).get('enabled',True) else None)
assert 'Select USB' in record['DIGU']['pdStart'].get('accessibleDescription','')
record['disabled_click']=b.request({'cmd':'invoke','target':'pdStart','action':'click'},5)
assert not panel()['pdStart']['enabled'] and not radio()['transmitting']
req({'cmd':'slice','action':'mode','value':'USB','id':'0'});wait(lambda:slices()[0]['mode']=='USB')
record['USB']=wait(lambda:panel() if panel().get('pdStart',{}).get('enabled') else None)
assert not radio()['transmitting'];record['radio_after']=radio();record['passed']=True
(p/'controls-proof.json').write_text(json.dumps(record,indent=2)+'\n');print('DIGU Start disabled with USB reason; USB readback enables explicit Start; no RF.');b.close()
