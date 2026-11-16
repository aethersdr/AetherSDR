import hashlib,json,os,select,signal,socket,time
from pathlib import Path
CASE='receive-stall'
EXPECTED={'aether-pd':'bed41f7b3f3308acdacf783e327a16d713a0b7466ac100c812b8b909d0716be8'}
report={'case':CASE,'probe_complete':False,'signals':[],'rf_commands':[],'runtime_restart_requested':False};started=time.monotonic();observer=None;handles=[]
def graph():
 rows=[]
 for p in Path('/proc').iterdir():
  if not p.name.isdecimal():continue
  try:
   exe=os.readlink(p/'exe')
   if '/aetherpd/' not in exe:continue
   f={l.split(':',1)[0]:l.split(':',1)[1].strip() for l in (p/'status').read_text().splitlines() if ':' in l}
   rows.append({'pid':int(p.name),'ppid':int(f['PPid']),'exe':exe,'uid':f['Uid'],'state':f['State'],'cgroup':(p/'cgroup').read_text().splitlines(),'start_ticks':int((p/'stat').read_text().rsplit(')',1)[1].split()[19]),'caps':{k:f[k] for k in ['CapInh','CapPrm','CapEff','CapBnd','CapAmb']},'nnp':f['NoNewPrivs']})
  except (FileNotFoundError,ProcessLookupError,PermissionError):pass
 return rows
def arp():return [l for l in Path('/proc/net/arp').read_text().splitlines()[1:] if l.split()[0]=='172.30.2.1']
def journal():
 p=Path('/run/.aetherpd-fixed-arp.8600');return {'exists':p.exists(),'size':p.stat().st_size if p.exists() else None,'sha256':hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None}
def interlock():
 global observer
 observer=socket.create_connection(('127.0.0.1',4992),2);observer.setblocking(False);observer.sendall(b'C1|sub tx all\n');buffer=b'';end=time.monotonic()+3
 while time.monotonic()<end:
  if not select.select([observer],[],[],.1)[0]:continue
  data=observer.recv(16384);assert data;buffer+=data;assert len(buffer)<131072
  while b'\n' in buffer:
   line,buffer=buffer.split(b'\n',1);text=line.decode('ascii').strip()
   if text.startswith('S') and '|interlock ' in text and '|interlock band ' not in text:
    fields=dict(v.split('=',1) for v in text.split('|',1)[1].split()[1:] if '=' in v)
    if all(k in fields for k in ['state','source','tx_client_handle','tx_allowed','reason']):
     assert fields['state']=='READY' and fields['source']=='' and fields['tx_client_handle']=='0x00000000' and fields['tx_allowed']=='1' and fields['reason']=='',fields
     return fields
 raise RuntimeError('Fresh idle interlock absent; no fault injected')

stopped=False;resumer=None;cancel_write=None
try:
 assert hashlib.sha256(Path('/var/lib/smartsdr/aetherpd/state/provision.json').read_bytes()).hexdigest()=='f511e5f09f41a99b0f5cd29e4e321eb6f93ec02162df41175ae6866173233cb5'
 assert Path('/var/lib/smartsdr/aetherpd/upgrade-generation').read_bytes()==b'PDGEN001'+bytes.fromhex('7a7d7a24553b31a10f8d260ef1cf81af11ff271fdaa2d61c5b1dafdfad395616')
 report['before']=graph();targets=[p for p in report['before'] if p['exe']=='/run/aetherpd/bin/aether-pd'];assert len(targets)==1;target=targets[0];p=Path('/proc')/str(target['pid']);fd=os.pidfd_open(target['pid']);handles.append(fd)
 assert int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==target['start_ticks']
 assert target['uid'].split()==['78666']*4 and target['nnp']=='1' and not any(int(v,16) for v in target['caps'].values())
 assert hashlib.sha256((p/'exe').read_bytes()).hexdigest()==EXPECTED['aether-pd'];report['idle_before']=interlock();observer.close();observer=None
 cancel_read,cancel_write=os.pipe();resumer=os.fork()
 if resumer==0:
  os.close(cancel_write)
  if not select.select([cancel_read],[],[],.5)[0]:
   try:signal.pidfd_send_signal(fd,signal.SIGCONT)
   except ProcessLookupError:pass
   os._exit(11)
  os._exit(0)
 os.close(cancel_read);stamp=time.monotonic_ns();signal.pidfd_send_signal(fd,signal.SIGSTOP);stopped=True;report['signals'].append({'pid':target['pid'],'start_ticks':target['start_ticks'],'signal':'SIGSTOP','monotonic_ns':stamp})
 deadline=time.monotonic()+.1
 while True:
  state=(p/'stat').read_text().rsplit(')',1)[1].split()[0]
  if state in ('T','t'):break
  assert time.monotonic()<deadline,'Stop state not observed';time.sleep(.001)
 report['stopped_state']=state;report['stop_observed_ns']=time.monotonic_ns();time.sleep(.15)
 signal.pidfd_send_signal(fd,signal.SIGCONT);stopped=False;stamp=time.monotonic_ns();report['signals'].append({'pid':target['pid'],'start_ticks':target['start_ticks'],'signal':'SIGCONT','monotonic_ns':stamp});os.write(cancel_write,b'x');os.close(cancel_write);cancel_write=None
 _,status=os.waitpid(resumer,0);resumer=None;report['resume_watchdog_exit']=os.waitstatus_to_exitcode(status);assert report['resume_watchdog_exit']==0,'Watchdog fallback needed'
 report['stopped_interval_ms']=(stamp-report['stop_observed_ns'])/1e6;time.sleep(1);report['after']=graph();same=[r for r in report['after'] if r['pid']==target['pid'] and r['start_ticks']==target['start_ticks']];report['same_DSP_alive']=bool(same)
 if same:assert not same[0]['state'].startswith(('T','t','Z'))
 report['idle_after']=interlock();report['arp_after']=arp();report['journal_after']=journal();report['accept_local']=Path('/proc/sys/net/ipv4/conf/eth0.10/accept_local').read_text().strip();assert report['accept_local']=='0';report['injection_and_idle_observation_passed']=True;report['probe_complete']=True
except Exception as error:report['error']=type(error).__name__+': '+str(error)
finally:
 if stopped:
  try:signal.pidfd_send_signal(fd,signal.SIGCONT)
  except ProcessLookupError:pass
 if cancel_write is not None:os.close(cancel_write)
 if resumer is not None:
  _,status=os.waitpid(resumer,0);report['fallback_watchdog_exit']=os.waitstatus_to_exitcode(status)
 for fd in handles:os.close(fd)
 if observer:observer.close()
 report['total_s']=round(time.monotonic()-started,3);print(json.dumps(report),flush=True)
