import hashlib,json,os,select,signal,socket,time
from pathlib import Path
CASE='intentional-GUI-disconnect'
EXPECTED={'aether-pd':'bed41f7b3f3308acdacf783e327a16d713a0b7466ac100c812b8b909d0716be8'}
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

def original_alive(row):
 try:return int((Path('/proc')/str(row['pid'])/'stat').read_text().rsplit(')',1)[1].split()[19])==row['start_ticks']
 except (FileNotFoundError,ProcessLookupError):return False

try:
 assert hashlib.sha256(Path('/var/lib/smartsdr/aetherpd/state/provision.json').read_bytes()).hexdigest()=='f511e5f09f41a99b0f5cd29e4e321eb6f93ec02162df41175ae6866173233cb5'
 assert Path('/var/lib/smartsdr/aetherpd/upgrade-generation').read_bytes()==b'PDGEN001'+bytes.fromhex('3478cb7a95ed460e67d0ce4e06c7962fe0b78e592f608ac598958bad7ecffc29')
 report['boot_id']=Path('/proc/sys/kernel/random/boot_id').read_text().strip();assert report['boot_id']=='839d29c7-c56e-4c61-a87e-05fdf1c6b6b6'
 report['before']=graph();dsps=[r for r in report['before'] if r['exe']=='/run/aetherpd/bin/aether-pd'];assert len(dsps)==1
 target=dsps[0];p=Path('/proc')/str(target['pid']);fd=os.pidfd_open(target['pid']);handles.append(fd);assert original_alive(target)
 assert target['uid'].split()==['78666']*4 and target['nnp']=='1' and not any(int(v,16) for v in target['caps'].values()) and '/aetherpd-start@' in str(target['cgroup'])
 assert hashlib.sha256((p/'exe').read_bytes()).hexdigest()==EXPECTED['aether-pd']
 env=dict(x.split(b'=',1) for x in (p/'environ').read_bytes().split(b'\0') if b'=' in x)
 assert env.get(b'AETHER_PD_GUI_HANDLE')==CONFIG['gui_handle'].encode() and env.get(b'AETHER_PD_DAXIQ')==b'4' and env.get(b'AETHER_PD_FEEDBACK_PAN')==CONFIG['pan'].encode()
 workers={'aether-pd','pd-iq-source','pd-capture-bootstrap','pd-fixed-arp','pd-unkey-guard','pd-lease-guard'}
 cohort=[r for r in report['before'] if r['cgroup']==target['cgroup']];originals=[r for r in cohort if Path(r['exe']).name in workers];assert len(originals)==9
 report['target']=target;report['original_worker_pid_checks_including_zombies']=originals;report['idle_before']=interlock();report['ARP_before']=arp();assert len(report['ARP_before'])==1
 stamp=time.monotonic_ns();report['observer_ready_ns']=stamp
 with socket.create_connection((CONFIG['host'],CONFIG['port']),3) as notice:
  notice.settimeout(8);notice.sendall((json.dumps({'nonce':CONFIG['nonce'],'ready':True})+'\n').encode());data=b''
  while b'\n' not in data:
   part=notice.recv(4096);assert part;data+=part;assert len(data)<8192
  ack=json.loads(data);assert ack['nonce']==CONFIG['nonce'] and ack['disconnected_observed'] and ack['pid']==2296936;report['disconnect_ack']={k:v for k,v in ack.items() if k!='nonce'}
 ack_stamp=time.monotonic_ns();report['disconnect_ack_ns']=ack_stamp;deadline=time.monotonic()+25;changes=[];previous=None
 while True:
  rows=graph();active=[r for r in rows if r['cgroup']==target['cgroup'] and Path(r['exe']).name in workers];survivors=[r for r in originals if original_alive(r)]
  state=[(r['pid'],r['state']) for r in active]
  if state!=previous:changes.append({'after_ack_ms':(time.monotonic_ns()-ack_stamp)/1e6,'active':active});previous=state
  if not active and not survivors:break
  assert time.monotonic()<deadline,('Original workers remain',survivors);time.sleep(.05)
 report['workers_gone_after_ack_ms']=(time.monotonic_ns()-ack_stamp)/1e6;report['workers_gone_after_observer_ready_ms']=(time.monotonic_ns()-stamp)/1e6;report['changes']=changes
 time.sleep(1);report['after']=graph();assert not any(Path(r['exe']).name in workers for r in report['after'])
 report['ARP_after']=arp();assert not report['ARP_after'];report['journal_after']=journal();assert report['journal_after']['size']==0
 observer.close();observer=None;report['idle_after']=interlock();report['accept_local']=Path('/proc/sys/net/ipv4/conf/eth0.10/accept_local').read_text().strip();assert report['accept_local']=='0';report['probe_complete']=True
except Exception as error:report['error']=type(error).__name__+': '+str(error)
finally:
 for fd in handles:os.close(fd)
 if observer:observer.close()
 report['total_s']=round(time.monotonic()-started,3);print(json.dumps(report),flush=True)
