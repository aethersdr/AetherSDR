import hashlib,json,os,select,signal,socket,time
from pathlib import Path
CASE='owned-unkey-api-loss'
EXPECTED={'aether-pd':'bed41f7b3f3308acdacf783e327a16d713a0b7466ac100c812b8b909d0716be8','pd-unkey-guard':'d945aeaf3926c51e63a2054d64f4bd799334d453cc575e060ec37464cadf7662'}
CONFIG=None
report={'case':CASE,'probe_complete':False,'signals':[],'rf_commands':[],'runtime_restart_requested':False,'commands':[]};started=time.monotonic();observer=None;handles=[]
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

def validate(row):
 p=Path('/proc')/str(row['pid']);assert original_alive(row)
 assert row['uid'].split()==['78666']*4 and row['nnp']=='1' and not any(int(v,16) for v in row['caps'].values())
 assert hashlib.sha256((p/'exe').read_bytes()).hexdigest()==EXPECTED[Path(row['exe']).name]

try:
 assert hashlib.sha256(Path('/var/lib/smartsdr/aetherpd/state/provision.json').read_bytes()).hexdigest()=='f511e5f09f41a99b0f5cd29e4e321eb6f93ec02162df41175ae6866173233cb5'
 assert Path('/var/lib/smartsdr/aetherpd/upgrade-generation').read_bytes()==b'PDGEN001'+bytes.fromhex('56a1b78e03441fd6ff9eac942e7b8aa9831c46381a4511666cfefebf33602956')
 report['boot_id']=Path('/proc/sys/kernel/random/boot_id').read_text().strip();assert report['boot_id']=='839d29c7-c56e-4c61-a87e-05fdf1c6b6b6'
 report['before']=graph();dsps=[r for r in report['before'] if r['exe']=='/run/aetherpd/bin/aether-pd'];assert len(dsps)==1
 dsp=dsps[0];validate(dsp);assert '/aetherpd-start@' in str(dsp['cgroup'])
 cohort=[r for r in report['before'] if r['cgroup']==dsp['cgroup']];guards=[r for r in cohort if r['exe']=='/run/aetherpd/bin/pd-unkey-guard'];assert len(guards)==1
 target=guards[0];validate(target);p=Path('/proc')/str(target['pid']);args=(p/'cmdline').read_bytes().rstrip(b'\0').decode('ascii').split('\0')
 assert len(args)==5 and args[1:3]==['3','4'] and int(args[4],16)==CONFIG['gui_handle'] and int(args[3],16)>0 and int(args[3],16)!=CONFIG['gui_handle']
 target['guard_argv']=args;handle=int(args[3],16);report['target']=target
 link=os.readlink(p/'fd/3');assert link.startswith('socket:[') and link.endswith(']');inode=link[8:-1]
 tcp=[r.split() for r in Path('/proc/net/tcp').read_text().splitlines()[1:]];links=[r for r in tcp if r[9]==inode];assert len(links)==1
 assert links[0][2]=='0100007F:1380' and links[0][3]=='01',links;report['target_api_socket']={'inode':inode,'local':links[0][1],'peer':links[0][2],'state':links[0][3]}
 fd=os.pidfd_open(target['pid']);handles.append(fd);validate(target)
 report['idle_before']=interlock()
 # FlexLib Radio.DisconnectClientByHandle; never use the argumentless all-GUI form.
 command='C91|client disconnect 0x%08x\n'%handle;stamp=time.monotonic_ns();observer.sendall(command.encode('ascii'));report['commands'].append({'text':command.strip(),'monotonic_ns':stamp,'target_pid':target['pid'],'target_start_ticks':target['start_ticks']})
 buffer=b'';deadline=time.monotonic()+4;reply=None
 while time.monotonic()<deadline:
  if not select.select([observer],[],[],.05)[0]:continue
  data=observer.recv(16384);assert data;buffer+=data;assert len(buffer)<131072
  while b'\n' in buffer:
   line,buffer=buffer.split(b'\n',1);text=line.decode('ascii',errors='strict').strip()
   if text.startswith('R91|'):reply=text;break
  if reply is not None:break
 assert reply is not None and reply.split('|')[1]=='0',('Disconnect rejected',reply);report['disconnect_ack']=reply;report['ack_ms']=(time.monotonic_ns()-stamp)/1e6
 poll=select.poll();poll.register(fd,select.POLLIN);assert poll.poll(4000),'Owned unkey guard did not exit after API disconnect';report['guard_exit_ms']=(time.monotonic_ns()-stamp)/1e6
 workers={'aether-pd','pd-fixed-arp','pd-iq-source','pd-capture-bootstrap','pd-unkey-guard','pd-lease-guard'}
 originals=[r for r in cohort if Path(r['exe']).name in workers];deadline=time.monotonic()+8
 while True:
  survivors=[r for r in originals if original_alive(r)]
  if not survivors:break
  assert time.monotonic()<deadline,('Original workers including zombies remain',survivors);time.sleep(.02)
 report['original_worker_pid_checks_including_zombies']=originals;report['workers_gone_ms']=(time.monotonic_ns()-stamp)/1e6
 time.sleep(1);report['after']=graph();assert not any(Path(r['exe']).name in workers for r in report['after'])
 report['arp_after']=arp();assert not report['arp_after'];report['journal_after']=journal();assert report['journal_after']['size']==0
 observer.close();observer=None;report['idle_after']=interlock();report['accept_local']=Path('/proc/sys/net/ipv4/conf/eth0.10/accept_local').read_text().strip();assert report['accept_local']=='0'
 report['probe_complete']=True
except Exception as error:report['error']=type(error).__name__+': '+str(error)
finally:
 for fd in handles:os.close(fd)
 if observer:observer.close()
 report['total_s']=round(time.monotonic()-started,3);print(json.dumps(report),flush=True)
