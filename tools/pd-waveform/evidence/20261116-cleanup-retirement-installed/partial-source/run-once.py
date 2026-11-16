"""One receive-only GUI start, a fixed early IQ-source pause, no retry/restart."""
import ast,json,pathlib,secrets,socket,subprocess,sys,time
ROOT=pathlib.Path(__file__).resolve().parent;BASE=ROOT.parent;sys.path.insert(0,str(BASE.parent/'aetherpd-source/tools'));from automation_probe import Bridge
assert not (ROOT/'events.json').exists(),'Preserve each named attempt'
b=Bridge('/tmp/aetherpd-cleanup-retirement');events=[];started=time.monotonic();result={'passed':False,'RF_commands':[],'Start_actions':0};job=None
listener=socket.socket();listener.bind(('0.0.0.0',0));listener.listen(1);listener.settimeout(.5)
def record(kind,**kw):
 events.append({'event':kind,'monotonic_ns':time.monotonic_ns(),'elapsed_s':round(time.monotonic()-started,3),**kw});(ROOT/'events.json').write_text(json.dumps(events,indent=2)+'\n')
def req(q):
 r=b.request(q,5);record('bridge',request=q,result=r);assert r.get('ok'),r;return r
def status():
 p=subprocess.run([str(BASE/'first-iq-live/vault-v3'),'aetherpd/status'],capture_output=True,text=True,timeout=15);r=json.loads(p.stdout);record('service_status',status=r);return r
def controls():
 r=b.request({'cmd':'dumpTree','target':'AetherPDDialog'},5);assert r.get('ok'),r;out={}
 def walk(x):
  if isinstance(x,dict):
   if str(x.get('objectName','')).startswith('pd'):out[x['objectName']]={k:x[k] for k in ['value','enabled','toolTip','accessibleDescription'] if k in x}
   for v in x.values():walk(v)
  elif isinstance(x,list):
   for v in x:walk(v)
 walk(r);return out
try:
 ident=req({'cmd':'whoami'});assert ident['pid']==2177039 and ident['guiClientId'].upper()=='5D14B8D4-A745-5620-BAF8-B3614CA1C814'
 radio=req({'cmd':'get','model':'radio'})['radio'];assert radio['connected'] and not radio['transmitting'] and not radio['fullDuplex'] and radio['serial']=='2125-1213-8600-8895'
 slices=req({'cmd':'get','model':'slices'})['slices'];assert len(slices)==1 and slices[0]['mode']=='USB'
 pans=req({'cmd':'get','model':'pans'})['pans'];assert len(pans)==1 and pans[0]['ownedByUs']  and pans[0]['rxAntenna']=='RX_A' and pans[0]['rfGain']==0
 before=status();assert before.get('body',{}).get('available') is False;req({'cmd':'invoke','target':'pdIqSelection','action':'setCurrentIndex','value':1})
 route=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);route.connect(('192.168.50.100',4992));config={'host':route.getsockname()[0],'port':listener.getsockname()[1],'nonce':secrets.token_hex(24),'gui_handle':pans[0]['clientHandle'],'pan':pans[0]['panId']};route.close()
 source=(ROOT/'fault-template.py').read_text().replace('CONFIG=None','CONFIG='+repr(config),1);ast.parse(source);(ROOT/'pause-job.py').write_text(source);log=(ROOT/'orchestration.log').open('w');job=subprocess.Popen([sys.executable,str(ROOT/'run-job.py'),'pause'],stdout=log,stderr=subprocess.STDOUT)
 deadline=time.monotonic()+120
 while True:
  try:
   conn,_=listener.accept();conn.settimeout(2)
   with conn:
    raw=b''
    while b'\n' not in raw:
     part=conn.recv(4096);assert part;raw+=part;assert len(raw)<8192
   ready=json.loads(raw);assert ready['nonce']==config['nonce'] and ready['ready'];record('fault_observer_ready');break
  except socket.timeout:
   assert job.poll() is None,'Observer ended before readiness';assert time.monotonic()<deadline
 result['Start_actions']=1;req({'cmd':'invoke','target':'pdStart','action':'click'});record('receive_only_start_intent');deadline=time.monotonic()+60;seen_registered=False
 while True:
  r=req({'cmd':'get','model':'radio'})['radio'];assert r['connected'] and not r['transmitting'] and not r['fullDuplex']
  s=status();seen_registered|=s.get('body',{}).get('registered',False)
  if job.poll() is not None:break
  assert time.monotonic()<deadline;time.sleep(.3)
 assert job.returncode==0;outer=json.loads((ROOT/'pause-result.json').read_text());probe=json.loads(outer['logs']);record('probe_result',result=probe);assert outer['wait']['StatusCode']==0 and outer['job_container_deleted'] and outer['temporary_host_directory_deleted'] and probe['probe_complete'],probe
 assert not seen_registered,'DSP became registered during forced IQ startup failure';s=status();assert not s.get('body',{}).get('available',False)
 result.update(probe=probe,registered_was_observed=seen_registered,status_after=s,controls_after=controls(),radio_after=req({'cmd':'get','model':'radio'})['radio'],passed=True)
except Exception as e:result['error']=type(e).__name__+': '+str(e)
finally:
 if job and job.poll() is None:
  try:job.wait(timeout=60)
  except subprocess.TimeoutExpired:result['job_still_running']=True;result['passed']=False
 listener.close();b.close();(ROOT/'outcome.json').write_text(json.dumps(result,indent=2)+'\n');print({k:result[k] for k in ['passed','Start_actions','error'] if k in result})
 if not result['passed']:raise SystemExit(1)
