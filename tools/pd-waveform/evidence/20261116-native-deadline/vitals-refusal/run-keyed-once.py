"""One explicit5W GUI key, pause desktop only, observe native deadline with DSP alive."""
import ast,hashlib,importlib.util,json,os,pathlib,secrets,signal,socket,subprocess,sys,time
ROOT=pathlib.Path(__file__).resolve().parent;BASE=ROOT.parent;W=BASE.parent/'aetherpd-source'
sys.path.insert(0,str(W/'tools'));from automation_probe import Bridge
spec=importlib.util.spec_from_file_location('bench_common',BASE/'stock-candidate/bench-common.py');common=importlib.util.module_from_spec(spec);spec.loader.exec_module(common)
prior=json.loads((BASE/'desktop-stall-keyed-02/keyed-events.json').read_text());assert time.monotonic_ns()-prior[-1]['monotonic_ns']>=31_000_000_000,'RF cooldown'
assert not (ROOT/'keyed-events.json').exists(),'One invocation only; preserve failed attempts'
b=Bridge('/tmp/aetherpd-usb-start');events=[];start=time.monotonic();keyed=False;job=None;observer=None;listener=None
result={'passed':False,'positive_GUI_key_actions':0};desktop_fd=None;stopped=False;watchdog=None;cancel_write=None;meter=None
mspec=importlib.util.spec_from_file_location('meter_watch',ROOT/'meter-watch.py');mw=importlib.util.module_from_spec(mspec);mspec.loader.exec_module(mw)
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
 proc=pathlib.Path('/proc/2037172');desktop_fd=os.pidfd_open(2037172)
 assert int((proc/'stat').read_text().rsplit(')',1)[1].split()[19])==5992572 and hashlib.sha256((proc/'exe').read_bytes()).hexdigest()=='571e811bfd871c83adabd47cda863809c44add9e0b82f0086c4813c4e3e7f847'
 record('desktop_pin',pid=2037172,start_ticks=5992572,sha256='571e811bfd871c83adabd47cda863809c44add9e0b82f0086c4813c4e3e7f847')
 radio=req({'cmd':'get','model':'radio'})['radio'];assert radio['connected'] and not radio['transmitting'] and not radio['fullDuplex'] and radio['serial']=='2125-1213-8600-8895'
 tx=req({'cmd':'get','model':'transmit'})['transmit'];assert tx['rfPower']==5 and tx['maxPowerLevel']<=10 and not tx['apdEnabled']
 slices=req({'cmd':'get','model':'slices'})['slices'];assert len(slices)==1 and slices[0]['mode']=='PD' and slices[0]['txAntenna']=='ANT1' and slices[0]['rxAntenna']=='RX_A' and slices[0]['frequency']==7.15
 pans=req({'cmd':'get','model':'pans'})['pans'];assert len(pans)==1 and pans[0]['ownedByUs'] and pans[0]['rfGain']==0 and pans[0]['centerMhz']==7.15;gui=int(pans[0]['clientHandle'],16)
 s=status();assert s['http_status']==200 and s['body']['registered'] and not s['body']['keyed'];binding=s['body']['feedback_binding'];assert binding['valid'] and binding['transport']=='local_dax_iq' and binding['gui_handle']==gui and binding['channel']==4
 meter=mw.MeterWatch();end=time.monotonic()+5
 while not meter.ready():
  assert time.monotonic()<end,'Independent meter stream not ready';time.sleep(.02)
 record('independent_meters_ready',snapshot=meter.snapshot())
 metadata=req({'cmd':'get','model':'meters'})['meters']['all'];pins={(x['index'],x['name'],x['unit']) for x in metadata};assert {(4,'+13.8A','Volts'),(8,'FWDPWR','dBm'),(9,'REFPWR','dBm'),(10,'SWR','SWR'),(11,'PATEMP','degC')}<=pins
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
 cancel_read,cancel_write=os.pipe();key_request=time.monotonic();watchdog=subprocess.Popen([sys.executable,str(ROOT/'resume-watchdog.py'),str(desktop_fd),str(cancel_read),str(key_request+10),str(ROOT/'watchdog.json')],pass_fds=(desktop_fd,cancel_read));os.close(cancel_read)
 result['positive_GUI_key_actions']=1;invoke('pdMeasureTransmit');keyed=True;deadline=key_request+9.5;saw_tx=False;idle_while_stopped=False;guards_ack=False
 listener.settimeout(.01)
 while time.monotonic()<deadline:
  snap=observer.snapshot();inter=snap['interlock'];record('independent_observer',snapshot=snap)
  if inter.get('state')=='TRANSMITTING':
   assert int(inter['tx_client_handle'],16)==gui;saw_tx=True
  record('independent_power_guard',values=meter.check(int(key_request*1e9)))
  if not guards_ack:
   try:conn,_=listener.accept()
   except socket.timeout:conn=None
   if conn:
    with conn:
     conn.settimeout(1);raw=b''
     while b'\n' not in raw:
      part=conn.recv(4096);assert part;raw+=part;assert len(raw)<16384
     notice=json.loads(raw);assert notice['nonce']==config['nonce'] and notice['phase']=='guards_validated' and saw_tx
     signal.pidfd_send_signal(desktop_fd,signal.SIGSTOP);stopped=True;end=time.monotonic()+.5
     while True:
      state=(proc/'stat').read_text().rsplit(')',1)[1].split()[0]
      if state in ('T','t'):break
      assert time.monotonic()<end,'Desktop stop not observed';time.sleep(.001)
     stamp=time.monotonic_ns();record('desktop_stopped',pid=2037172,start_ticks=5992572,state=state,stop_observed_ns=stamp)
     conn.sendall((json.dumps({'nonce':config['nonce'],'desktop_stopped':True,'pid':2037172,'start_ticks':5992572,'stop_observed_ns':stamp})+'\n').encode());guards_ack=True
  if stopped:
   state=(proc/'stat').read_text().rsplit(')',1)[1].split()[0];record('desktop_still_stopped',state=state);assert state in ('T','t'),'Desktop resumed before unkey observation'
   if inter.get('state')=='READY' and int(inter['tx_client_handle'],16)==0:
    idle_while_stopped=True;record('unkey_before_desktop_resume',snapshot=snap,desktop_state=state);break
  elif saw_tx and inter.get('state')=='READY':raise RuntimeError('RF ended before isolation; no retry')
  time.sleep(.02)
 assert saw_tx and idle_while_stopped and guards_ack
 signal.pidfd_send_signal(desktop_fd,signal.SIGCONT);stopped=False;record('desktop_resumed',pid=2037172)
 os.write(cancel_write,b'x');os.close(cancel_write);cancel_write=None;watchdog.wait(timeout=2);result['watchdog']=json.loads((ROOT/'watchdog.json').read_text());assert result['watchdog']['action']=='canceled'
 result['unkey_observed_while_desktop_stopped']=True;result['saw_key']=True
 job.wait(timeout=60);assert job.returncode==0
 receipt=json.loads((ROOT/'keyed-result.json').read_text());fault=json.loads(receipt['logs']);record('fault_result',result=fault)
 assert receipt['wait']['StatusCode']==0 and receipt['job_container_deleted'] and receipt['temporary_host_directory_deleted'] and fault['passed'],fault
 assert not fault['signals'] and fault['same_DSP_alive'] and fault['bounded_guard_original_PID_gone'];result['fault']=fault;result['passed']=True
except Exception as e:result['error']=type(e).__name__+': '+str(e)
finally:
 if stopped and desktop_fd is not None:
  signal.pidfd_send_signal(desktop_fd,signal.SIGCONT);stopped=False;record('emergency_desktop_resume',pid=2037172)
 if cancel_write is not None:
  os.write(cancel_write,b'x');os.close(cancel_write);cancel_write=None
 if watchdog:
  try:watchdog.wait(timeout=2)
  except subprocess.TimeoutExpired:result['watchdog_pending']=True;result['passed']=False
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
 if desktop_fd is not None:os.close(desktop_fd)
 if meter:
  (ROOT/'independent-meters.json').write_text(json.dumps({'port':meter.port,'replies':meter.replies,'rows':meter.rows},indent=2)+'\n');meter.close()
 if listener:listener.close()
 if observer:record('observer_rows',rows=observer.rows);observer.close()
 b.close();(ROOT/'outcome.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:result[k] for k in ['passed','cleanup_error','positive_GUI_key_actions'] if k in result} | ({'error':result['error'][:500]} if 'error' in result else {})),flush=True)
 if not result['passed']:raise SystemExit(1)
