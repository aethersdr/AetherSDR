import json,pathlib,sys,time
sys.path.insert(0,'/home/jeremy/AetherSDR/build/aetherpd-source/tools');from automation_probe import Bridge
out=pathlib.Path(__file__).resolve().parent;b=Bridge('/tmp/aetherpd-first-iq-fresh')
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
who=request({'cmd':'whoami'});assert not who['txAllowed']
r=request({'cmd':'get','model':'radio'})['radio'];assert r['connected'] and not r['transmitting'] and not r['fullDuplex'] and r['serial']=='2125-1213-8600-8895'
request({'cmd':'invoke','target':'pdInstallFirst','action':'click'});end=time.monotonic()+30
while True:
 p=panel()
 if p['pdInstallApprove']['enabled']:break
 assert time.monotonic()<end,p;time.sleep(.2)
manifest=json.loads((out.parent.parent/'first-iq-distribution/short/manifest.json').read_text());text=p['pdInstallPlan']['value']
assert manifest['release']['blake2b_256'] in text and manifest['selection']['publisher_key'] in text
(out/'prepared.json').write_text(json.dumps({'whoami':who,'radio':r,'panel':p,'approved':False,'package_source':'88280414febdaa69c42f4a7190a38b3f94b90238'},indent=2)+'\n');print(json.dumps(p,indent=2));b.close()
