import json,pathlib,sys,time
sys.path.insert(0,'/home/jeremy/AetherSDR/build/aetherpd-source/tools');from automation_probe import Bridge
out=pathlib.Path(__file__).resolve().parent;target=out/'install-result.json';assert not target.exists()
prepared=json.loads((out/'prepared.json').read_text());b=Bridge('/tmp/aetherpd-acquisition')
def request(q):
 r=b.request(q,10);assert r.get('ok'),r;return r
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
who=request({'cmd':'whoami'});assert who['pid']==json.loads((out/'instance.json').read_text())['pid'];r=request({'cmd':'get','model':'radio'})['radio'];assert r['connected'] and not r['transmitting'] and not r['fullDuplex'] and r['serial']=='2125-1213-8600-8895'
p=panel();assert p['pdInstallApprove']['enabled'] and p['pdInstallPlan']['value']==prepared['panel']['pdInstallPlan']['value']
report={'whoami':who,'radio_before':r,'plan_before':p,'rf_commands':[],'old_store_absent':False,'predecessor_credential_used_for_delivery':True,'candidate_source':'ba00b01c43676ac60d4f05ff1e76fae629a02e2e','diagnostics_used_for_delivery':False,'states':[],'committed':False}
start=time.monotonic();target.write_text(json.dumps(report,indent=2)+'\n')
try:
 report['approval']=request({'cmd':'invoke','target':'pdInstallApprove','action':'click'});end=start+180;previous=None
 while True:
  p=panel();state=p['pdInstallState'].get('value','')
  if state!=previous:
   row={'elapsed_s':round(time.monotonic()-start,3),'state':state};report['states'].append(row);print(json.dumps(row),flush=True);previous=state
  report['latest']=p;report['elapsed_s']=round(time.monotonic()-start,3);target.write_text(json.dumps(report,indent=2)+'\n')
  if 'radio confirmed' in state.lower() or 'committed' in state.lower():report['committed']=True;break
  if any(word in state.lower() for word in ['could not','unconfirmed','refused','failed']):break
  assert time.monotonic()<end,'installation observation deadline';time.sleep(.5)
 report['radio_after']=request({'cmd':'get','model':'radio'})
finally:
 target.write_text(json.dumps(report,indent=2)+'\n');b.close()
assert report['committed'],report['latest']
