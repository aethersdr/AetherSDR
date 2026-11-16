import hashlib,json,os,select,signal,socket,time
from pathlib import Path
CASE='partial-start'
EXPECTED={'pd-iq-source':'37fa974279793ccb0b4ee5891bc361a536d1cc917a12521ffe8d09781cc35ac0'}
CONFIG=None
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

stopped=False;resumer=None;cancel_write=None;fd=None
try:
 assert hashlib.sha256(Path('/var/lib/smartsdr/aetherpd/state/provision.json').read_bytes()).hexdigest()=='f511e5f09f41a99b0f5cd29e4e321eb6f93ec02162df41175ae6866173233cb5'
 assert Path('/var/lib/smartsdr/aetherpd/upgrade-generation').read_bytes()==b'PDGEN001'+bytes.fromhex('7a7d7a24553b31a10f8d260ef1cf81af11ff271fdaa2d61c5b1dafdfad395616')
 report['before']=graph();assert not any(Path(p['exe']).name in ['aether-pd','pd-iq-source','pd-capture-bootstrap','pd-fixed-arp'] for p in report['before']);report['idle_before']=interlock();observer.close();observer=None
 assert not arp();report['journal_before']=journal()
 with socket.create_connection((CONFIG['host'],CONFIG['port']),3) as notice:notice.sendall((json.dumps({'nonce':CONFIG['nonce'],'ready':True})+'\n').encode())
 deadline=time.monotonic()+30;target=None
 while time.monotonic()<deadline:
  rows=graph();sources=[p for p in rows if p['exe']=='/run/aetherpd/bin/pd-iq-source']
  if sources:
   assert len(sources)==1;candidate=sources[0]
   if candidate['uid'].split()==['78666']*4 and candidate['nnp']=='1':target=candidate;break
  time.sleep(.003)
 assert target is not None,'No owned source appeared; no signal sent'
 p=Path('/proc')/str(target['pid']);fd=os.pidfd_open(target['pid']);handles.append(fd);assert int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==target['start_ticks']
 assert '/aetherpd-start@' in str(target['cgroup']) and not any(int(v,16) for v in target['caps'].values());assert hashlib.sha256((p/'exe').read_bytes()).hexdigest()==EXPECTED['pd-iq-source']
 env=dict(x.split(b'=',1) for x in (p/'environ').read_bytes().split(b'\0') if b'=' in x)
 assert env.get(b'SHIM_PD_GUI_HANDLE')==b'0x484c0026' and env.get(b'SHIM_PD_GUI_CLIENT_ID',b'').upper()==b'5D14B8D4-A745-5620-BAF8-B3614CA1C814' and env.get(b'SHIM_PD_IQ_CHANNEL')==b'4' and env.get(b'SHIM_PD_FEEDBACK_PAN')==b'0x40000000'
 report['at_fault']=graph();cohort=[r for r in report['at_fault'] if r['cgroup']==target['cgroup']];assert not any(Path(r['exe']).name in ['aether-pd','pd-capture-bootstrap'] for r in cohort),'Startup already passed source stage; no signal sent'
 report['target']=target;cancel_read,cancel_write=os.pipe();resumer=os.fork()
 if resumer==0:
  os.close(cancel_write)
  if not select.select([cancel_read],[],[],12)[0]:
   try:signal.pidfd_send_signal(fd,signal.SIGCONT)
   except ProcessLookupError:pass
   os._exit(11)
  os._exit(0)
 os.close(cancel_read);stamp=time.monotonic_ns();signal.pidfd_send_signal(fd,signal.SIGSTOP);stopped=True;report['signals'].append({'pid':target['pid'],'start_ticks':target['start_ticks'],'signal':'SIGSTOP','monotonic_ns':stamp})
 end=time.monotonic()+.1
 while True:
  state=(p/'stat').read_text().rsplit(')',1)[1].split()[0]
  if state in ('T','t'):break
  assert time.monotonic()<end;time.sleep(.001)
 report['stopped_state']=state;report['stop_observed_ns']=time.monotonic_ns();time.sleep(8)
 report['during_pause']=graph();assert not any(Path(r['exe']).name in ['aether-pd','pd-capture-bootstrap'] for r in report['during_pause'] if r['cgroup']==target['cgroup']),'DSP/capture started without IQ handoff'
 try:signal.pidfd_send_signal(fd,signal.SIGCONT)
 except ProcessLookupError:report['source_already_retired']=True
 stopped=False;stamp=time.monotonic_ns();report['signals'].append({'pid':target['pid'],'start_ticks':target['start_ticks'],'signal':'SIGCONT','monotonic_ns':stamp});report['hold_ms']=(stamp-report['stop_observed_ns'])/1e6;os.write(cancel_write,b'x');os.close(cancel_write);cancel_write=None;_,status=os.waitpid(resumer,0);resumer=None;report['resume_watchdog_exit']=os.waitstatus_to_exitcode(status);assert report['resume_watchdog_exit']==0
 deadline=time.monotonic()+8
 while True:
  rows=graph();workers=[r for r in rows if Path(r['exe']).name in ['aether-pd','pd-iq-source','pd-capture-bootstrap','pd-fixed-arp','pd-unkey-guard','pd-lease-guard']]
  try:original=int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==target['start_ticks']
  except (FileNotFoundError,ProcessLookupError):original=False
  if not workers and not original:break
  assert time.monotonic()<deadline,('Startup workers remain',workers,original);time.sleep(.02)
 report['workers_gone_ms']=(time.monotonic_ns()-report['stop_observed_ns'])/1e6;report['after']=rows;report['original_source_PID_including_zombie_gone']=True;report['arp_after']=arp();report['journal_after']=journal();assert not report['arp_after'] and report['journal_after']['size']==0
 report['idle_after']=interlock();report['accept_local']=Path('/proc/sys/net/ipv4/conf/eth0.10/accept_local').read_text().strip();assert report['accept_local']=='0';report['probe_complete']=True
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
