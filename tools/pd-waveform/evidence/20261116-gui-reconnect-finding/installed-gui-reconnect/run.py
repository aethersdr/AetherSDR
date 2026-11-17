"""Intentional GUI disconnect and same-address reconnect; no RF or credentials changed."""
import ast,json,pathlib,secrets,socket,subprocess,sys,time
ROOT=pathlib.Path(__file__).resolve().parent;BASE=ROOT.parent;sys.path.insert(0,str(BASE.parent/'aetherpd-source/tools'));from automation_probe import Bridge
assert not (ROOT/'outcome.json').exists(),'Preserve each attempt'
b=Bridge('/tmp/aetherpd-prepared-child-exit');events=[];report={'passed':False,'RF_commands':[],'credential_mutations':[],'Start_actions':0};job=None;started=False;disconnected=False
listener=socket.socket();listener.bind(('0.0.0.0',0));listener.listen(1);listener.settimeout(.5)

def record(kind,**kw):
 events.append({'event':kind,'monotonic_ns':time.monotonic_ns(),**kw});(ROOT/'events.json').write_text(json.dumps(events,indent=2)+'\n')
def req(q):
 r=b.request(q,8);record('bridge',request=q,result=r);assert r.get('ok'),r;return r

def status():
 p=subprocess.run([str(BASE/'first-iq-live/vault-v3'),'aetherpd/status'],capture_output=True,text=True,timeout=22);r=json.loads(p.stdout);record('service_status',result=r);return r

def panel():
 r=b.request({'cmd':'dumpTree','target':'AetherPDDialog'},5);record('panel_tree',result=r);result={}
 if not r.get('ok'):return result
 def visit(x):
  if isinstance(x,dict):
   if str(x.get('objectName','')).startswith('pd'):result[x['objectName']]={k:x[k] for k in ['value','enabled','toolTip','accessibleDescription'] if k in x}
   for v in x.values():visit(v)
  elif isinstance(x,list):
   for v in x:visit(v)
 visit(r);return result

def stale_panel():
 controls=panel()
 for name in ['pdStart','pdStop','pdCalibrationArm','pdMeasurementArm']:
  if name in controls:assert controls[name].get('enabled') is False,(name,controls[name])
 result=b.request({'cmd':'invoke','target':'pdStart','action':'click'},5);record('stale_Start_refusal',result=result);assert not result.get('ok'),result
 return {'controls':controls,'stale_Start_refusal':result}

def connect_again():
 global disconnected
 req({'cmd':'connect','action':'local','value':'serial 2125-1213-8600-8895'});deadline=time.monotonic()+30
 while True:
  r=req({'cmd':'get','model':'radio'})['radio']
  if r['connected']:break
  assert time.monotonic()<deadline,r;time.sleep(.2)
 assert r['serial']=='2125-1213-8600-8895' and not r['transmitting'] and not r['fullDuplex'];disconnected=False;return r

try:
 who=req({'cmd':'whoami'});assert who['pid']==2296936 and who['guiClientId']=='5D14B8D4-A745-5620-BAF8-B3614CA1C814';report['whoami']=who
 radio=req({'cmd':'get','model':'radio'})['radio'];assert radio['connected'] and not radio['transmitting'] and not radio['fullDuplex'];report['radio_before']=radio
 pans=req({'cmd':'get','model':'pans'})['pans'];assert len(pans)==1 and pans[0]['ownedByUs'] and pans[0]['rxAntenna']=='RX_A' and pans[0]['rfGain']==0;report['pans_before']=pans
 slices=req({'cmd':'get','model':'slices'})['slices'];assert len(slices)==1 and slices[0]['mode']=='USB';assert status()['body'].get('state')=='stopped'
 req({'cmd':'invoke','target':'pdIqSelection','action':'setCurrentIndex','value':1});req({'cmd':'invoke','target':'pdStart','action':'click'});started=True;report['Start_actions']+=1;deadline=time.monotonic()+35
 while True:
  s=status()
  if s['http_status']==200 and s['body'].get('registered'):break
  assert time.monotonic()<deadline,s;time.sleep(.5)
 binding=s['body']['feedback_binding'];assert binding['valid'] and binding['channel']==4 and binding['gui_handle']==int(pans[0]['clientHandle'],16) and binding['pan']==int(pans[0]['panId'],16) and not s['body']['keyed'] and not s['body']['correcting'];report['registered_before']=s
 route=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);route.connect(('192.168.50.100',4992));config={'host':route.getsockname()[0],'port':listener.getsockname()[1],'nonce':secrets.token_hex(24),'gui_handle':pans[0]['clientHandle'],'pan':pans[0]['panId']};route.close()
 src=(ROOT/'observer-template.py').read_text().replace('CONFIG=None','CONFIG='+repr(config),1);ast.parse(src);(ROOT/'observe-job.py').write_text(src);log=(ROOT/'orchestration.log').open('w');job=subprocess.Popen([sys.executable,str(ROOT/'run-job.py'),'observe'],stdout=log,stderr=subprocess.STDOUT)
 end=time.monotonic()+120;conn=None
 while conn is None:
  try:conn,_=listener.accept()
  except socket.timeout:assert job.poll() is None and time.monotonic()<end,'Observer unavailable; no disconnect requested'
 conn.settimeout(8);data=b''
 while b'\n' not in data:
  part=conn.recv(4096);assert part;data+=part;assert len(data)<8192
 notice=json.loads(data);assert notice['ready'] and notice['nonce']==config['nonce'];record('observer_ready')
 req({'cmd':'disconnect'});disconnected=True;deadline=time.monotonic()+8
 while True:
  r=req({'cmd':'get','model':'radio'})['radio']
  if not r['connected']:break
  assert time.monotonic()<deadline,r;time.sleep(.05)
 report['disconnect_observed']=r;conn.sendall((json.dumps({'nonce':config['nonce'],'disconnected_observed':True,'pid':who['pid']})+'\n').encode());conn.close();record('disconnect_acknowledged')
 report['stale_while_disconnected']=stale_panel()
 # Do not call the HTTP PD status API here: it renews the private channel's15s idle lifetime.
 deadline=time.monotonic()+90
 while job.poll() is None:
  r=req({'cmd':'get','model':'radio'})['radio'];assert not r['connected'],'Unexpected reconnect';assert time.monotonic()<deadline;time.sleep(.5)
 assert job.returncode==0;outer=json.loads((ROOT/'observe-result.json').read_text());probe=json.loads(outer['logs']);report['probe']=probe;assert outer['wait']['StatusCode']==0 and outer['job_container_deleted'] and outer['temporary_host_directory_deleted'] and probe['probe_complete'],probe
 started=False;report['status_after_retirement']=status();assert not report['status_after_retirement'].get('body',{}).get('available',False)
 report['radio_reconnected']=connect_again();end=time.monotonic()+15
 while True:
  new=req({'cmd':'get','model':'pans'})['pans']
  if len(new)==1 and new[0]['ownedByUs']:break
  assert time.monotonic()<end,new;time.sleep(.2)
 report['pans_after']=new;assert new[0]['clientHandle']!=pans[0]['clientHandle'] and new[0]['rxAntenna']=='RX_A' and new[0]['rfGain']==0
 report['stale_after_reconnect']=stale_panel();s=status();assert not s.get('body',{}).get('registered',False);report['status_after_stale_click']=s
 controls=panel()
 if controls:req({'cmd':'invoke','target':'pdDone','action':'click'})
 req({'cmd':'invoke','target':'Waveforms...','action':'trigger'});req({'cmd':'invoke','target':'openAetherPD','action':'click'});report['new_panel_opened']=True;report['passed']=True
except Exception as e:report['error']=type(e).__name__+': '+str(e)
finally:
 if job and job.poll() is None:
  try:job.wait(timeout=60)
  except subprocess.TimeoutExpired:report['observer_still_running']=True;report['passed']=False
 if disconnected:
  try:report['fallback_reconnect']=connect_again()
  except Exception as e:report['fallback_reconnect_error']=str(e)
 if started and not disconnected and not report.get('observer_still_running'):
  try:
   controls=panel()
   if controls:req({'cmd':'invoke','target':'pdDone','action':'click'})
   req({'cmd':'invoke','target':'Waveforms...','action':'trigger'});req({'cmd':'invoke','target':'openAetherPD','action':'click'});time.sleep(.5);report['fallback_Stop']=req({'cmd':'invoke','target':'pdStop','action':'click'})
  except Exception as e:report['fallback_Stop_error']=str(e)
 listener.close();b.close();(ROOT/'outcome.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:report[k] for k in ['passed','Start_actions','error','observer_still_running','fallback_reconnect_error','fallback_Stop_error'] if k in report}),flush=True)
 if not report['passed']:raise SystemExit(1)
