"""Installed runtime auth/ownership boundary probe; no RF or identity mutations."""
import concurrent.futures,http.client,json,pathlib,subprocess,sys,time
ROOT=pathlib.Path(__file__).resolve().parent;BASE=ROOT.parent
sys.path.insert(0,str(BASE.parent/'aetherpd-source/tools'));from automation_probe import Bridge
assert not (ROOT/'outcome.json').exists(),'Preserve each attempt'
b=Bridge('/tmp/aetherpd-cleanup-retirement');events=[];report={'passed':False,'RF_commands':[],'credential_mutations':[],'start_actions':0};started=False

def record(event,**kw):
 events.append({'event':event,'monotonic_ns':time.monotonic_ns(),**kw});(ROOT/'events.json').write_text(json.dumps(events,indent=2)+'\n')
def bridge(q):
 r=b.request(q,5);record('bridge',request=q,response=r);assert r.get('ok'),r;return r

def authenticated(path):
 p=subprocess.run([str(BASE/'first-iq-live/vault-v3'),path],capture_output=True,text=True,timeout=22)
 r=json.loads(p.stdout);record('authenticated_read',path=path,response=r);return r

def boundary(action,identity):
 p=subprocess.run([str(ROOT/'vault-ownership'),action],input=json.dumps(identity),capture_output=True,text=True,timeout=25)
 assert p.returncode==0,(p.returncode,p.stderr)
 r=json.loads(p.stdout);record('authenticated_boundary',action=action,response=r);return r

def bad_auth(method,path,authorization):
 conn=http.client.HTTPConnection('192.168.50.100',48992,timeout=8)
 headers={'Content-Type':'application/json'}
 if authorization is not None:headers['Authorization']=authorization
 conn.request(method,path,body=b'{}' if method=='POST' and path.endswith('/start') else None,headers=headers)
 response=conn.getresponse();raw=response.read(65537);assert len(raw)<=65536
 result={'method':method,'path':path,'authorization_kind':'missing' if authorization is None else 'invalid','http_status':response.status,'body':json.loads(raw)};conn.close();record('unauthorized_boundary',**result);assert result['http_status']==401,result;return result

def idle():
 radio=bridge({'cmd':'get','model':'radio'})['radio'];assert radio['connected'] and not radio['transmitting'] and not radio['fullDuplex'] and radio['serial']=='2125-1213-8600-8895';return radio

def running():
 r=authenticated('aetherpd/status');assert r['http_status']==200 and r['body'].get('registered') and not r['body']['keyed'] and not r['body']['correcting'],r
 return r['body']

def capture(identity):
 r=authenticated('aetherpd/capture');assert r['http_status']==200,r
 c=r['body'];v=c['feedback_binding'];assert v['valid'] and not v['failed'] and v['transport']=='local_dax_iq' and v['channel']==4 and v['gui_handle']==identity['gui_handle'] and v['pan']==identity['feedback_pan']
 assert c['sample_count']==16384 and c['sample_rate_hz']==48000 and not c['keyed'] and not c['correcting'];return c

def stop():
 global started
 bridge({'cmd':'invoke','target':'pdStop','action':'click'})
 deadline=time.monotonic()+20
 while True:
  s=authenticated('aetherpd/status')
  if s['http_status']==200 and s['body'].get('state')=='stopped':break
  assert time.monotonic()<deadline,s;time.sleep(.25)
 started=False;return s
try:
 who=bridge({'cmd':'whoami'});assert who['pid']==2177039 and who['guiClientId']=='5D14B8D4-A745-5620-BAF8-B3614CA1C814'
 report['radio_before']=idle();pans=bridge({'cmd':'get','model':'pans'})['pans'];assert len(pans)==1 and pans[0]['ownedByUs'] and pans[0]['rxAntenna']=='RX_A' and pans[0]['rfGain']==0
 slices=bridge({'cmd':'get','model':'slices'})['slices'];assert len(slices)==1 and slices[0]['mode']=='USB'
 identity={'gui_client_id':who['guiClientId'],'gui_handle':int(pans[0]['clientHandle'],16),'iq_channel':4,'feedback_pan':int(pans[0]['panId'],16)};report['identity']=identity
 assert authenticated('aetherpd/status')['body'].get('state')=='stopped'
 bridge({'cmd':'invoke','target':'pdIqSelection','action':'setCurrentIndex','value':1});bridge({'cmd':'invoke','target':'pdStart','action':'click'});started=True;report['start_actions']+=1
 deadline=time.monotonic()+35
 while True:
  s=authenticated('aetherpd/status')
  if s['http_status']==200 and s['body'].get('registered'):break
  assert time.monotonic()<deadline,s;time.sleep(.5)
 report['before']=running();report['capture_before']=capture(identity)
 report['auth_rejections']=[]
 for auth in [None,'Bearer invalid-aetherpd-probe']:
  for method,path in [('GET','/v1/aetherpd/status'),('POST','/v2/aetherpd/start'),('POST','/v1/aetherpd/stop'),('GET','/v1/aetherpd/installation'),('POST','/v3/aetherpd/capture')]:
   report['auth_rejections'].append(bad_auth(method,path,auth))
  running();idle()
 report['duplicate_start']=boundary('duplicate',identity);assert report['duplicate_start']['http_status']==409
 with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:report['concurrent_duplicate_starts']=list(pool.map(lambda _:boundary('duplicate',identity),range(2)))
 assert all(r['http_status']==409 for r in report['concurrent_duplicate_starts'])
 report['unknown_executable_field']=boundary('malformed',identity);assert report['unknown_executable_field']['http_status']==400
 report['after_rejections']=running();report['capture_after']=capture(identity);assert report['capture_before']['capture_id']!=report['capture_after']['capture_id'];idle()
 report['stop_before_foreign']=stop()
 report['foreign_gui_start']=boundary('foreign',identity);assert report['foreign_gui_start']['http_status']==503
 # Invalid ownership must never publish an available or registered runtime.
 for _ in range(4):
  s=authenticated('aetherpd/status');assert not s.get('body',{}).get('available',False) and not s.get('body',{}).get('registered',False),s;time.sleep(.25)
 report['after_foreign']=s;report['radio_after']=idle();report['passed']=True
except Exception as e:report['error']=type(e).__name__+': '+str(e)
finally:
 if started:
  try:report['final_stop']=stop()
  except Exception as e:report['stop_error']=str(e);report['passed']=False
 b.close();(ROOT/'outcome.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:report[k] for k in ['passed','error','stop_error','start_actions'] if k in report}),flush=True)
 if not report['passed']:raise SystemExit(1)
