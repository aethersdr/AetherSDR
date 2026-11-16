"""One 5 W GUI-keyed bench experiment, with no automatic retries or rekey."""
import json,os,pathlib,signal,subprocess,sys,time
p=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(p.parents[1]/'aetherpd-source/tools'))
from automation_probe import Bridge
output=p/'stall-01-events.json';assert not output.exists(),'Never repeat a named RF attempt'
instance=json.loads((p/'instance.json').read_text());config=json.loads((p/'observer-config.json').read_text())
proc=pathlib.Path('/proc')/str(instance['pid']);assert int((proc/'stat').read_text().rsplit(')',1)[1].split()[19])==config['start_ticks']
pidfd=os.pidfd_open(instance['pid']);events=[];start=time.monotonic();observer=None;attempted=False;failure=None
assert json.loads((p/'preflight.json').read_text())['passed']
assert json.loads((p/'preflight-observer.json').read_text())['completed']
def record(event,**data):
 events.append({'event':event,'elapsed_s':time.monotonic()-start,'monotonic_ns':time.monotonic_ns(),**data})
 temp=output.with_suffix('.tmp');temp.write_text(json.dumps(events,indent=2));temp.replace(output)
def request(q,save=True):
 b=Bridge(instance['socket'])
 try:r=b.request(q,5)
 finally:b.close()
 if save:record('bridge',request=q,result=r)
 assert r['ok'],r
 return r
def invoke(n,a='click',v=None):
 q={'cmd':'invoke','target':n,'action':a}
 if v is not None:q['value']=v
 return request(q)
def widgets():
 out={}
 def visit(x):
  if isinstance(x,dict):
   if x.get('objectName','').startswith('pdMeasure'):out[x['objectName']]={k:v for k,v in x.items() if k in ('value','enabled','visible')}
   for v in x.values():visit(v)
  elif isinstance(x,list):
   for v in x:visit(v)
 visit(request({'cmd':'dumpTree','target':'AetherPDMeasurementDialog'},False));return out
def enabled(n,seconds=6):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  w=widgets()
  if w.get(n,{}).get('enabled'):return w
  time.sleep(.1)
 record('not_ready',widgets=w);raise RuntimeError(n+' not enabled')
def status():
 q=subprocess.run([str(p.parent/'first-iq-live/vault-v3'),'aetherpd/status'],capture_output=True,text=True,timeout=15)
 r=json.loads(q.stdout);record('service_status',result=r);assert r['http_status']==200;return r['body']
try:
 identity=request({'cmd':'whoami'});assert identity['pid']==instance['pid'] and identity['txAllowed']
 r=request({'cmd':'get','model':'radio'})['radio'];assert r['connected'] and not r['transmitting'] and not r['fullDuplex'] and r['serial']=='2125-1213-8600-8895'
 tx=request({'cmd':'get','model':'transmit'})['transmit'];assert tx['rfPower']==5 and tx['maxPowerLevel']<=10 and not tx['apdEnabled']
 slices=request({'cmd':'get','model':'slices'})['slices'];assert len(slices)==1 and slices[0]['frequency']==7.15 and slices[0]['txAntenna']=='ANT1' and slices[0]['rxAntenna']=='RX_A' and slices[0]['mode']=='PD'
 pans=request({'cmd':'get','model':'pans'})['pans'];assert len(pans)==1 and pans[0]['centerMhz']==7.15 and pans[0]['rfGain']==0 and int(pans[0]['clientHandle'],16)==config['gui_handle']
 body=status();assert body['registered'] and not body['keyed'] and body['correction_enabled'] and body['feedback_binding']['valid'] and body['feedback_binding']['channel']==4 and body['feedback_binding']['gui_handle']==config['gui_handle']
 invoke('pdMeasureDummyLoad','setChecked','true');invoke('pdMeasurePowerLimit','setValue',10)
 assert widgets()['pdMeasureTimeout']['value']=='10'
 enabled('pdMeasureArm');invoke('pdMeasureArm');end=time.monotonic()+10
 while True:
  body=status()
  if body.get('calibration_state')=='armed' and body.get('source_purpose')=='measurement':break
  assert time.monotonic()<end;time.sleep(.2)
 assert not body['keyed'] and body['measurement']['source_id']!='0000000000000000'
 enabled('pdMeasurePrepare');invoke('pdMeasurePrepare');record('receive_ready',widgets=enabled('pdMeasureTransmit'))
 observer=subprocess.Popen([sys.executable,str(p/'observer.py'),'stall-01'],start_new_session=True,stdout=open(p/'stall-01-observer.log','x'),stderr=subprocess.STDOUT)
 end=time.monotonic()+5
 while not (p/'stall-01-observer-ready.json').exists():
  assert observer.poll() is None and time.monotonic()<end,'Observer did not become ready';time.sleep(.05)
 tx=request({'cmd':'get','model':'transmit'})['transmit'];assert tx['rfPower']==5 and tx['maxPowerLevel']<=10 and not tx['apdEnabled']
 record('operator_key_intent',frequency_mhz=7.15,nominal_watts=5,configured_maximum_watts=10,source_seconds=8,native_cleanup_interval_seconds=9,desktop_authorization_seconds=10,desktop_pause_seconds=11,one_positive_GUI_action=True)
 attempted=True;invoke('pdMeasureTransmit')
 # The independent observer handles its own resume. Avoid GUI calls during the stall.
 end=time.monotonic()+20
 while observer.poll() is None and time.monotonic()<end:time.sleep(.1)
 assert observer.poll() is not None,'Observer exceeded experiment deadline'
 report=json.loads((p/'stall-01-observer.json').read_text());record('observer_outcome',report=report)
 assert observer.returncode==0,report.get('error')
except BaseException as e:
 failure=repr(e);record('failure',error=failure)
finally:
 signal.pidfd_send_signal(pidfd,signal.SIGCONT);os.close(pidfd)
 if observer is not None and observer.poll() is None:
  observer.terminate()
  try:observer.wait(3)
  except subprocess.TimeoutExpired:observer.kill();observer.wait(2)
 try:
  r=request({'cmd':'get','model':'radio'})['radio']
  if r['transmitting']:
   invoke('pdMeasureStop');request({'cmd':'key','action':'ptt','value':'off'})
  end=time.monotonic()+8
  while True:
   w=widgets();r=request({'cmd':'get','model':'radio'},False)['radio']
   if not r['transmitting'] and not w.get('pdMeasureStop',{}).get('enabled'):break
   if time.monotonic()>=end:break
   time.sleep(.2)
  body=status();record('final_state',radio=r,widgets=w,service=body)
  assert not r['transmitting'] and not body['keyed'],'Radio not confirmed idle'
  if w.get('pdMeasureCancelSource',{}).get('enabled'):invoke('pdMeasureCancelSource')
 except BaseException as e:
  record('cleanup_error',error=repr(e));failure=failure or repr(e)
 record('end',attempted=attempted,error=failure,no_automatic_rearm=True)
print(json.dumps({'attempted':attempted,'error':failure,'events_file':str(output)}))
if failure:raise SystemExit(1)
