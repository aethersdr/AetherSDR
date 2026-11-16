import hashlib,json,os,select,signal,socket,time
from pathlib import Path
CASE='capture-startup-exit'
EXPECTED={'pd-capture-bootstrap':'b3132499052fdb709c64fa0267171072299aae21fe0a970c0debc4648e40eccc'}
CONFIG={'host': '192.168.50.221', 'port': 56089, 'nonce': 'a2e4db12f04c8a4ec60e9620d892522392d1cffe13017ea9', 'gui_handle': '0x5ecc4ab6', 'pan': '0x40000000'}
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

def original_alive(row):
 try:return int((Path('/proc')/str(row['pid'])/'stat').read_text().rsplit(')',1)[1].split()[19])==row['start_ticks']
 except (FileNotFoundError,ProcessLookupError):return False

try:
 assert hashlib.sha256(Path('/var/lib/smartsdr/aetherpd/state/provision.json').read_bytes()).hexdigest()=='f511e5f09f41a99b0f5cd29e4e321eb6f93ec02162df41175ae6866173233cb5'
 assert Path('/var/lib/smartsdr/aetherpd/upgrade-generation').read_bytes()==b'PDGEN001'+bytes.fromhex('3478cb7a95ed460e67d0ce4e06c7962fe0b78e592f608ac598958bad7ecffc29')
 report['boot_id']=Path('/proc/sys/kernel/random/boot_id').read_text().strip();assert report['boot_id']=='839d29c7-c56e-4c61-a87e-05fdf1c6b6b6'
 report['before']=graph();workers={'aether-pd','pd-iq-source','pd-capture-bootstrap','pd-fixed-arp','pd-unkey-guard','pd-lease-guard'}
 assert not any(Path(r['exe']).name in workers for r in report['before']);report['idle_before']=interlock();observer.close();observer=None;assert not arp()
 with socket.create_connection((CONFIG['host'],CONFIG['port']),3) as notice:notice.sendall((json.dumps({'nonce':CONFIG['nonce'],'ready':True})+'\n').encode())
 deadline=time.monotonic()+30;target=None
 while time.monotonic()<deadline:
  rows=graph();by_pid={r['pid']:r for r in rows}
  for row in rows:
   if row['exe']!='/run/aetherpd/bin/pd-capture-bootstrap' or row['uid'].split()!=['65534']*4 or any(int(v,16) for v in row['caps'].values()):continue
   parent=by_pid.get(row['ppid']);
   if parent and parent['exe']=='/run/aetherpd/bin/pd-radio-launch' and '/aetherpd-start@' in str(row['cgroup']):target=row;break
  if target:break
  time.sleep(.001)
 assert target is not None,'Capture constructor did not appear; no signal sent'
 p=Path('/proc')/str(target['pid']);fd=os.pidfd_open(target['pid']);handles.append(fd);assert original_alive(target)
 assert hashlib.sha256((p/'exe').read_bytes()).hexdigest()==EXPECTED['pd-capture-bootstrap']
 parent=Path('/proc')/str(target['ppid']);assert hashlib.sha256((parent/'exe').read_bytes()).hexdigest()=='09c9103a47581d829c61ff33fa180556ccda2e3a4071ed1951fb643e0c838877'
 env=dict(x.split(b'=',1) for x in (p/'environ').read_bytes().split(b'\0') if b'=' in x)
 assert env.get(b'SHIM_PD_GUI_HANDLE')==CONFIG['gui_handle'].encode() and env.get(b'SHIM_PD_GUI_CLIENT_ID',b'').upper()==b'5D14B8D4-A745-5620-BAF8-B3614CA1C814' and env.get(b'SHIM_PD_IQ_CHANNEL')==b'4' and env.get(b'SHIM_PD_FEEDBACK_PAN')==CONFIG['pan'].encode()
 report['at_fault']=graph();cohort=[r for r in report['at_fault'] if r['cgroup']==target['cgroup']];assert not any(Path(r['exe']).name=='aether-pd' for r in cohort),'DSP already appeared; no fault injected'
 report['target']=target;stamp=time.monotonic_ns();signal.pidfd_send_signal(fd,signal.SIGKILL);report['signals'].append({'pid':target['pid'],'start_ticks':target['start_ticks'],'signal':'SIGKILL','monotonic_ns':stamp})
 originals={r['pid']:r for r in cohort if Path(r['exe']).name in workers};deadline=time.monotonic()+17;observations=[];previous=None
 while True:
  rows=graph();active=[r for r in rows if r['cgroup']==target['cgroup'] and Path(r['exe']).name in workers]
  for row in active:originals.setdefault(row['pid'],row)
  survivors=[r for r in originals.values() if original_alive(r)]
  launchers=[r for r in rows if r['pid']==target['ppid']]
  view={'elapsed_ms':(time.monotonic_ns()-stamp)/1e6,'launcher':launchers,'active_workers':active}
  state=[(r['pid'],r['exe'],r['uid'],r['state']) for r in launchers+active]
  if state!=previous:observations.append(view);previous=state
  if not active and not survivors:break
  assert time.monotonic()<deadline,('Startup workers remain',survivors);time.sleep(.02)
 report['workers_gone_ms']=(time.monotonic_ns()-stamp)/1e6;assert report['workers_gone_ms']<2000,('Startup retirement exceeded2s regression bound',report['workers_gone_ms']);report['observations']=observations;report['original_worker_pid_checks_including_zombies']=list(originals.values())
 time.sleep(1);report['after']=graph();assert not any(Path(r['exe']).name in workers for r in report['after'])
 report['arp_after']=arp();assert not report['arp_after'];report['journal_after']=journal();assert report['journal_after']['size']==0
 report['idle_after']=interlock();report['accept_local']=Path('/proc/sys/net/ipv4/conf/eth0.10/accept_local').read_text().strip();assert report['accept_local']=='0';report['probe_complete']=True
except Exception as error:report['error']=type(error).__name__+': '+str(error)
finally:
 for fd in handles:os.close(fd)
 if observer:observer.close()
 report['total_s']=round(time.monotonic()-started,3);print(json.dumps(report),flush=True)
