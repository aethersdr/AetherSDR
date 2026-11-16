import hashlib,json,os,select,signal,socket,time
from pathlib import Path
CONFIG=None
report={'passed':False,'signals':[],'rf_commands':[],'events':[]};handles=[];observer=None
EXPECTED={'aether-pd':'bed41f7b3f3308acdacf783e327a16d713a0b7466ac100c812b8b909d0716be8','pd-unkey-guard':'d945aeaf3926c51e63a2054d64f4bd799334d453cc575e060ec37464cadf7662'}
def graph():
 rows=[]
 for p in Path('/proc').iterdir():
  if not p.name.isdecimal():continue
  try:
   exe=os.readlink(p/'exe')
   if '/aetherpd/' not in exe:continue
   f=dict(l.split(':',1) for l in (p/'status').read_text().splitlines() if ':' in l)
   rows.append({'pid':int(p.name),'ppid':int(f['PPid']),'exe':exe,'uid':f['Uid'].split(),'state':f['State'].strip(),'cgroup':(p/'cgroup').read_text().splitlines(),'start_ticks':int((p/'stat').read_text().rsplit(')',1)[1].split()[19]),'caps':{k:f[k].strip() for k in ['CapInh','CapPrm','CapEff','CapBnd','CapAmb']},'nnp':f['NoNewPrivs'].strip()})
  except (FileNotFoundError,ProcessLookupError,PermissionError):pass
 return rows
def validate(row):
 assert row['uid']==['78666']*4 and row['nnp']=='1' and not any(int(v,16) for v in row['caps'].values()),row
 assert hashlib.sha256((Path('/proc')/str(row['pid'])/'exe').read_bytes()).hexdigest()==EXPECTED[Path(row['exe']).name]
def read_interlocks(seconds):
 global buf
 end=time.monotonic()+seconds;events=[]
 while time.monotonic()<end:
  if not select.select([observer],[],[],min(.01,max(0,end-time.monotonic())))[0]:continue
  data=observer.recv(16384);assert data,'Observer disconnected';buf+=data;assert len(buf)<131072
  while b'\n' in buf:
   line,buf=buf.split(b'\n',1)
   if line.startswith(b'S') and b'|interlock ' in line and b'|interlock band ' not in line:
    f=dict(v.split('=',1) for v in line.decode('ascii').strip().split('|',1)[1].split()[1:] if '=' in v)
    if 'state' in f:
     row={'monotonic_ns':time.monotonic_ns(),'fields':f};report['events'].append(row);events.append(row)
 return events
try:
 assert hashlib.sha256(Path('/var/lib/smartsdr/aetherpd/state/provision.json').read_bytes()).hexdigest()=='f511e5f09f41a99b0f5cd29e4e321eb6f93ec02162df41175ae6866173233cb5'
 assert Path('/var/lib/smartsdr/aetherpd/upgrade-generation').read_bytes()==b'PDGEN001'+bytes.fromhex('7a7d7a24553b31a10f8d260ef1cf81af11ff271fdaa2d61c5b1dafdfad395616')
 report['before']=graph();dsp=[p for p in report['before'] if p['exe']=='/run/aetherpd/bin/aether-pd'];assert len(dsp)==1;target=dsp[0];validate(target);group=target['cgroup'];assert len(group)==1 and '/aetherpd-start@' in group[0]
 fd=os.pidfd_open(target['pid']);handles.append(fd);assert int((Path('/proc')/str(target['pid'])/'stat').read_text().rsplit(')',1)[1].split()[19])==target['start_ticks']
 observer=socket.create_connection(('127.0.0.1',4992),2);observer.setblocking(False);observer.sendall(b'C1|sub tx all\n');buf=b'';first=read_interlocks(1)
 assert first and first[-1]['fields']['state']=='READY' and int(first[-1]['fields']['tx_client_handle'],16)==0
 with socket.create_connection((CONFIG['host'],CONFIG['port']),3) as notify:notify.sendall((json.dumps({'nonce':CONFIG['nonce'],'ready':True,'target':target})+'\n').encode())
 end=time.monotonic()+35;key=None
 while time.monotonic()<end and key is None:
  for e in read_interlocks(.03):
   if e['fields']['state']=='TRANSMITTING':
    assert int(e['fields']['tx_client_handle'],16)==CONFIG['gui_handle'] and e['fields'].get('source')=='SW',e
    key=e;break
 assert key is not None,'No operator transmission; no signal sent';report['key']=key
 # Two seconds of observed owned TX; a prior safety stop prevents injection.
 while time.monotonic_ns()-key['monotonic_ns']<2_000_000_000:
  for e in read_interlocks(.02):assert e['fields']['state']=='TRANSMITTING','RF ended before injection; no retry'
 report['at_fault']=graph();cohort=[p for p in report['at_fault'] if p['cgroup']==group]
 guards=[p for p in cohort if p['exe']=='/run/aetherpd/bin/pd-unkey-guard'];assert len(guards)==1,'Independent guard absent; no signal sent';validate(guards[0]);report['independent_guard']=guards[0]
 assert any(p['pid']==target['pid'] and p['start_ticks']==target['start_ticks'] for p in cohort)
 stamp=time.monotonic_ns();signal.pidfd_send_signal(fd,signal.SIGKILL);report['signals'].append({'pid':target['pid'],'start_ticks':target['start_ticks'],'signal':'SIGKILL','monotonic_ns':stamp})
 end=time.monotonic()+10;idle=None
 while time.monotonic()<end and idle is None:
  for e in read_interlocks(.02):
   if e['fields']['state']=='READY' and int(e['fields']['tx_client_handle'],16)==0:idle=e;break
 assert idle is not None,'No READY after DSP failure'
 report['unkey']=idle;report['unkey_after_signal_ms']=(idle['monotonic_ns']-stamp)/1e6;report['key_to_idle_ms']=(idle['monotonic_ns']-key['monotonic_ns'])/1e6
 assert report['key_to_idle_ms']<=9500,'Independent 9second cleanup bound exceeded'
 workers={'aether-pd','pd-fixed-arp','pd-iq-source','pd-capture-bootstrap','pd-unkey-guard','pd-lease-guard'}
 originals=[p for p in cohort if Path(p['exe']).name in workers];end=time.monotonic()+8
 while True:
  survivors=[]
  for row in originals:
   p=Path('/proc')/str(row['pid'])
   try:
    if int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==row['start_ticks']:survivors.append(row)
   except (FileNotFoundError,ProcessLookupError):pass
  if not survivors:break
  assert time.monotonic()<end,('Original workers including zombies remain',survivors);time.sleep(.02)
 report['workers_gone_after_signal_ms']=(time.monotonic_ns()-stamp)/1e6;report['original_worker_pid_checks_including_zombies']=originals
 report['arp']=[l for l in Path('/proc/net/arp').read_text().splitlines()[1:] if l.split()[0]=='172.30.2.1'];assert not report['arp'];report['arp_journal_bytes']=Path('/run/.aetherpd-fixed-arp.8600').stat().st_size;assert report['arp_journal_bytes']==0
 time.sleep(1);report['after']=graph();assert not any(Path(p['exe']).name in workers for p in report['after']);report['accept_local']=Path('/proc/sys/net/ipv4/conf/eth0.10/accept_local').read_text().strip();assert report['accept_local']=='0';report['passed']=True
except Exception as e:report['error']=type(e).__name__+': '+str(e)
finally:
 for fd in handles:os.close(fd)
 if observer:observer.close()
 print(json.dumps(report),flush=True)
