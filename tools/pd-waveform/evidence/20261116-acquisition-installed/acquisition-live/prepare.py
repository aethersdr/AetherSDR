import json,pathlib,sys,time
sys.path.insert(0,'/home/jeremy/AetherSDR/build/aetherpd-source/tools');from automation_probe import Bridge
out=pathlib.Path(__file__).resolve().parent;b=Bridge('/tmp/aetherpd-acquisition')
def request(q):
 r=b.request(q,8);assert r.get('ok'),r;return r
def panel():
 result={}
 def walk(x):
  if isinstance(x,dict):
   n=x.get('objectName','')
   if n.startswith('pdInstall'):result[n]={k:x[k] for k in ['value','enabled'] if k in x}
   for v in x.values():walk(v)
  elif isinstance(x,list):
   for v in x:walk(v)
 walk(request({'cmd':'dumpTree','target':'AetherPDDialog'}));return result
who=request({'cmd':'whoami'});assert who['pid']==json.loads((out/'instance.json').read_text())['pid']
r=request({'cmd':'get','model':'radio'})['radio']
if not r['connected']:
 request({'cmd':'connect','action':'local','value':'serial 2125-1213-8600-8895'})
 end=time.monotonic()+25
 while not r['connected']:
  assert time.monotonic()<end,'connection observation timeout';time.sleep(.2);r=request({'cmd':'get','model':'radio'})['radio']
request({'cmd':'invoke','target':'Waveforms...','action':'trigger'});time.sleep(.3)
request({'cmd':'invoke','target':'openAetherPD','action':'click'});time.sleep(.3)
assert r['connected'] and not r['transmitting'] and not r['fullDuplex'] and r['serial']=='2125-1213-8600-8895'
request({'cmd':'invoke','target':'pdInstallPrepare','action':'click'});end=time.monotonic()+30
while True:
 p=panel()
 if p['pdInstallApprove']['enabled']:break
 assert time.monotonic()<end,p;time.sleep(.2)
manifest=json.loads((out.parent/'upgrade-acquisition-timing/short/manifest.json').read_text());text=p['pdInstallPlan']['value']
assert 'd4cb44485dade35a82e1d4c4ef6845ae3cce28dfe2d4ffefbc9c8497c9bb47ef' in text and manifest['release']['blake2b_256'] in text and manifest['selection']['candidate_key'] in text
(out/'prepared.json').write_text(json.dumps({'whoami':who,'radio':r,'panel':p,'approved':False,'package_source':'ba00b01c43676ac60d4f05ff1e76fae629a02e2e'},indent=2)+'\n');print(json.dumps(p,indent=2));b.close()
