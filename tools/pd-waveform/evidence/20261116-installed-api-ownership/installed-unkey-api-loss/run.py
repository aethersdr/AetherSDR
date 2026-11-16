"""One owned non-GUI API disconnect; never RF, credentials or firmware service changes."""
import ast,json,pathlib,subprocess,sys,time
ROOT=pathlib.Path(__file__).resolve().parent;BASE=ROOT.parent
sys.path.insert(0,str(BASE.parent/'aetherpd-source/tools'));from automation_probe import Bridge
assert not (ROOT/'outcome.json').exists(),'Preserve each attempt'
b=Bridge('/tmp/aetherpd-cleanup-retirement');report={'passed':False,'RF_commands':[],'start_actions':0};events=[];job=None;started=False

def record(kind,**kw):
 events.append({'event':kind,'monotonic_ns':time.monotonic_ns(),**kw});(ROOT/'events.json').write_text(json.dumps(events,indent=2)+'\n')
def req(q):
 r=b.request(q,5);record('bridge',request=q,response=r);assert r.get('ok'),r;return r

def status():
 p=subprocess.run([str(BASE/'first-iq-live/vault-v3'),'aetherpd/status'],capture_output=True,text=True,timeout=22);r=json.loads(p.stdout);record('status',response=r);return r

def idle():
 r=req({'cmd':'get','model':'radio'})['radio'];assert r['connected'] and not r['transmitting'] and not r['fullDuplex'];return r
try:
 who=req({'cmd':'whoami'});assert who['pid']==2177039 and who['guiClientId']=='5D14B8D4-A745-5620-BAF8-B3614CA1C814'
 report['radio_before']=idle();pans=req({'cmd':'get','model':'pans'})['pans'];assert len(pans)==1 and pans[0]['ownedByUs'] and pans[0]['rxAntenna']=='RX_A' and pans[0]['rfGain']==0
 slices=req({'cmd':'get','model':'slices'})['slices'];assert len(slices)==1 and slices[0]['mode']=='USB'
 assert not status().get('body',{}).get('available',False)
 req({'cmd':'invoke','target':'pdStart','action':'click'});report['start_actions']+=1;started=True;end=time.monotonic()+35
 while True:
  s=status()
  if s['http_status']==200 and s['body'].get('registered'):break
  assert time.monotonic()<end,s;time.sleep(.5)
 assert not s['body']['keyed'] and not s['body']['correcting'];report['registered_before']=s
 binding=s['body']['feedback_binding'];assert binding['valid'] and binding['channel']==4 and binding['gui_handle']==int(pans[0]['clientHandle'],16) and binding['pan']==int(pans[0]['panId'],16)
 config={'gui_handle':int(pans[0]['clientHandle'],16)};source=(ROOT/'fault-template.py').read_text().replace('CONFIG=None','CONFIG='+repr(config),1);ast.parse(source);(ROOT/'disconnect-job.py').write_text(source)
 log=(ROOT/'orchestration.log').open('w');job=subprocess.Popen([sys.executable,str(ROOT/'run-job.py'),'disconnect'],stdout=log,stderr=subprocess.STDOUT)
 end=time.monotonic()+150
 while job.poll() is None:
  idle();assert time.monotonic()<end,'Observer job still active; do not duplicate';time.sleep(.5)
 assert job.returncode==0;outer=json.loads((ROOT/'disconnect-result.json').read_text());probe=json.loads(outer['logs']);report['probe']=probe;assert outer['wait']['StatusCode']==0 and outer['job_container_deleted'] and outer['temporary_host_directory_deleted'] and probe['probe_complete'],probe
 report['status_after']=status();assert not report['status_after'].get('body',{}).get('available',False) and not report['status_after'].get('body',{}).get('registered',False)
 report['radio_after']=idle();report['passed']=True;started=False
except Exception as e:report['error']=type(e).__name__+': '+str(e)
finally:
 if job and job.poll() is None:
  try:job.wait(timeout=60)
  except subprocess.TimeoutExpired:report['job_still_running']=True;report['passed']=False
 if started and not report.get('job_still_running'):
  try:report['fallback_stop']=req({'cmd':'invoke','target':'pdStop','action':'click'})
  except Exception as e:report['fallback_stop_error']=str(e)
 b.close();(ROOT/'outcome.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:report[k] for k in ['passed','error','start_actions','job_still_running'] if k in report}),flush=True)
 if not report['passed']:raise SystemExit(1)
