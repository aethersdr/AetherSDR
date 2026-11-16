"""One explicit 5W GUI key; fixed installed DSP fault only after owned TX and guard validation."""
import ast,importlib.util,json,os,pathlib,secrets,socket,subprocess,sys,time
ROOT=pathlib.Path(__file__).resolve().parent;BASE=ROOT.parent;W=BASE.parent/'aetherpd-source'
sys.path.insert(0,str(W/'tools'));from automation_probe import Bridge
spec=importlib.util.spec_from_file_location('bench_common',BASE/'stock-candidate/bench-common.py');common=importlib.util.module_from_spec(spec);spec.loader.exec_module(common)
assert not (ROOT/'keyed-events.json').exists(),'One invocation only; preserve failed attempts'
b=Bridge('/tmp/aetherpd-usb-start');events=[];start=time.monotonic();keyed=False;job=None;observer=None;listener=None
result={'passed':False,'positive_GUI_key_actions':0}
def record(event,**kw):
 events.append({'event':event,'elapsed_s':round(time.monotonic()-start,3),'monotonic_ns':time.monotonic_ns(),**kw});(ROOT/'keyed-events.json').write_text(json.dumps(events,indent=2)+'\n')
def req(q):
 r=b.request(q,5);record('bridge',request=q,result=r);assert r.get('ok'),r;return r
def invoke(n,a='click',v=None):
 q={'cmd':'invoke','target':n,'action':a}
 if v is not None:q['value']=v
 return req(q)
def widgets():
 r=b.request({'cmd':'dumpTree','target':'AetherPDMeasurementDialog'},5);assert r.get('ok'),r;out={}
 def walk(x):
  if isinstance(x,dict):
   if str(x.get('objectName','')).startswith('pdMeasure'):out[x['objectName']]={k:x[k] for k in ['value','enabled','toolTip','accessibleDescription'] if k in x}
   for z in x.values():walk(z)
  elif isinstance(x,list):
   for z in x:walk(z)
 walk(r);return out
def wait_enabled(n,seconds):
 end=time.monotonic()+seconds
 while True:
  w=widgets()
  if w.get(n,{}).get('enabled'):return w
  assert time.monotonic()<end,(n,w);time.sleep(.1)
def status():
 p=subprocess.run([str(BASE/'first-iq-live/vault-v3'),'aetherpd/status'],capture_output=True,text=True,timeout=12);s=json.loads(p.stdout);record('service_status',status=s);return s
try:
 identity=req({'cmd':'whoami'});assert identity['pid']==2037172 and identity['txAllowed']
 radio=req({'cmd':'get','model':'radio'})['radio'];assert radio['connected'] and not radio['transmitting'] and not radio['fullDuplex'] and radio['serial']=='2125-1213-8600-8895'
 tx=req({'cmd':'get','model':'transmit'})['transmit'];assert tx['rfPower']==5 and tx['maxPowerLevel']<=10 and not tx['apdEnabled']
 slices=req({'cmd':'get','model':'slices'})['slices'];assert len(slices)==1 and slices[0]['mode']=='PD' and slices[0]['txAntenna']=='ANT1' and slices[0]['rxAntenna']=='RX_A' and slices[0]['frequency']==7.15
 pans=req({'cmd':'get','model':'pans'})['pans'];assert len(pans)==1 and pans[0]['ownedByUs'] and pans[0]['rfGain']==0 and pans[0]['centerMhz']==7.15;gui=int(pans[0]['clientHandle'],16)
 s=status();assert s['http_status']==200 and s['body']['registered'] and not s['body']['keyed'];binding=s['body']['feedback_binding'];assert binding['valid'] and binding['transport']=='local_dax_iq' and binding['gui_handle']==gui and binding['channel']==4
 observer=common.Observer();snapshot=observer.snapshot();assert snapshot['interlock']['state']=='READY' and snapshot['apd']['enable']=='0';record('observer_before',snapshot=snapshot)
 invoke('pdMeasureDummyLoad','setChecked','true');invoke('pdMeasurePowerLimit','setValue',10)
 control=widgets();record('configured_controls',widgets=control)
 assert float(control['pdMeasureTimeout']['value'])==10 and float(control['pdMeasurePowerLimit']['value'])==10
 listener=socket.socket();listener.bind(('0.0.0.0',0));listener.listen(1);listener.settimeout(.5)
 route=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);route.connect(('192.168.50.100',4992));host=route.getsockname()[0];route.close()
 config={'host':host,'port':listener.getsockname()[1],'nonce':secrets.token_hex(24),'gui_handle':gui}
 source=(ROOT/'fault-template.py').read_text().replace('CONFIG=None','CONFIG='+repr(config),1);ast.parse(source);(ROOT/'keyed-job.py').write_text(source)
 joblog=(ROOT/'orchestration.log').open('w');job=subprocess.Popen([sys.executable,str(ROOT/'run-job.py'),'keyed'],stdout=joblog,stderr=subprocess.STDOUT)
 end=time.monotonic()+120;ready=None
 while time.monotonic()<end:
  try:
   conn,_=listener.accept();conn.settimeout(2)
   with conn:
    raw=b''
    while b'\n' not in raw:
     data=conn.recv(4096);assert data;raw+=data;assert len(raw)<16384
   notice=json.loads(raw);assert notice['nonce']==config['nonce'];ready={k:v for k,v in notice.items() if k!='nonce'};break
  except socket.timeout:
   assert job.poll() is None,'Fault observer ended before readiness'
 assert ready and ready['ready'];record('fault_observer_ready',notice=ready)
 # Arming and keying occur once, after the fault observer has validated the idle process.
 wait_enabled('pdMeasureArm',5);invoke('pdMeasureArm');end=time.monotonic()+8
 while True:
  s=status()
  if s.get('body',{}).get('calibration_state')=='armed' and s['body'].get('source_purpose')=='measurement':break
  assert time.monotonic()<end,s;time.sleep(.1)
 assert not s['body']['keyed'];wait_enabled('pdMeasurePrepare',3);invoke('pdMeasurePrepare');record('prepared_for_one_key',widgets=wait_enabled('pdMeasureTransmit',5))
 tx=req({'cmd':'get','model':'transmit'})['transmit'];radio=req({'cmd':'get','model':'radio'})['radio'];assert tx['rfPower']==5 and tx['maxPowerLevel']<=10 and not tx['apdEnabled'] and not radio['transmitting'] and not radio['fullDuplex']
 record('operator_key_intent',nominal_watts=5,frequency_mhz=7.15,source_limit_s=8,independent_cleanup_s=9,desktop_limit_s=10,one_GUI_action=True)
 result['positive_GUI_key_actions']=1;invoke('pdMeasureTransmit');keyed=True;end=time.monotonic()+12;saw_tx=False
 while time.monotonic()<end:
  r=req({'cmd':'get','model':'radio'})['radio'];record('observer',snapshot=observer.snapshot());saw_tx|=r['transmitting']
  if r['txPower'] is not None and r['txPower']>10:invoke('pdMeasureStop');raise RuntimeError('Measured forward power exceeded 10W')
  assert r['connected'] and not r['fullDuplex']
  if saw_tx and not r['transmitting']:break
  time.sleep(.08)
 assert saw_tx and not r['transmitting'];record('GUI_unkey_observed',widgets=widgets());result['saw_key']=saw_tx
 job.wait(timeout=60);assert job.returncode==0
 receipt=json.loads((ROOT/'keyed-result.json').read_text());fault=json.loads(receipt['logs']);record('fault_result',result=fault)
 assert receipt['wait']['StatusCode']==0 and receipt['job_container_deleted'] and receipt['temporary_host_directory_deleted'] and fault['passed'],fault
 assert len(fault['signals'])==1;result['fault']=fault;result['passed']=True
except Exception as e:result['error']=type(e).__name__+': '+str(e)
finally:
 try:
  r=req({'cmd':'get','model':'radio'})['radio']
  if r['transmitting']:
   invoke('pdMeasureStop');req({'cmd':'key','action':'ptt','value':'off'})
  end=time.monotonic()+12
  while True:
   r=req({'cmd':'get','model':'radio'})['radio']
   if not r['transmitting']:break
   assert time.monotonic()<end;time.sleep(.1)
  result['radio_after']=r;result['status_after']=status();record('measurement_state_after',widgets=widgets())
 except Exception as e:result['cleanup_error']=str(e);result['passed']=False
 if job and job.poll() is None:
  try:job.wait(timeout=65)
  except subprocess.TimeoutExpired:result['job_still_running']=True;result['passed']=False
 if listener:listener.close()
 if observer:record('observer_rows',rows=observer.rows);observer.close()
 b.close();(ROOT/'outcome.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:result[k] for k in ['passed','error','cleanup_error','positive_GUI_key_actions'] if k in result}),flush=True)
 if not result['passed']:raise SystemExit(1)
