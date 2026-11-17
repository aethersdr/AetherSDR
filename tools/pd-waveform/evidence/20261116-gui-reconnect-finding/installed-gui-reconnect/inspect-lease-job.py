import hashlib,json,os,select,socket,time,subprocess
from pathlib import Path
report={'RF_commands':[],'signals':[],'passed':False,'processes':[]};observer=None
try:
 for p in Path('/proc').iterdir():
  if not p.name.isdecimal():continue
  try:
   exe=os.readlink(p/'exe')
   if '/aetherpd/' not in exe:continue
   f={x.split(':',1)[0]:x.split(':',1)[1].strip() for x in (p/'status').read_text().splitlines() if ':' in x}
   row={'pid':int(p.name),'ppid':int(f['PPid']),'exe':exe,'uid':f['Uid'],'caps':{k:f[k] for k in ['CapInh','CapPrm','CapEff','CapBnd','CapAmb']},'NNP':f['NoNewPrivs'],'sha256':hashlib.sha256((p/'exe').read_bytes()).hexdigest(),'argv':(p/'cmdline').read_bytes().rstrip(b'\0').decode().split('\0')};report['processes'].append(row)
   if exe.endswith('/pd-lease-guard'):
    assert row['uid'].split()==['78666']*4 and row['sha256']=='297183f7504ffd6c9c6d9f4713976a2bcbfc1df5d9caa788c402fd8568c0954e';row['fd_roles']={x.name:os.readlink(x) for x in (p/'fd').iterdir()};row['start_ticks']=int((p/'stat').read_text().rsplit(')',1)[1].split()[19])
  except (FileNotFoundError,ProcessLookupError,PermissionError):pass
 observer=socket.create_connection(('127.0.0.1',4992),2);observer.setblocking(False);observer.sendall(b'C1|sub tx all\nC2|sub slice all\nC3|slice list\n');data=b'';end=time.monotonic()+3
 while time.monotonic()<end:
  if not select.select([observer],[],[],.1)[0]:continue
  part=observer.recv(16384)
  if not part:break
  data+=part;assert len(data)<=262144
 report['API_lines']=[x.decode('ascii') for x in data.splitlines() if x[:1] in [b'R',b'H',b'V'] or b'|interlock ' in x or b'|slice ' in x]
 report['ARP']=[l for l in Path('/proc/net/arp').read_text().splitlines()[1:] if l.split()[0]=='172.30.2.1'];report['ARP_journal_bytes']=Path('/run/.aetherpd-fixed-arp.8600').stat().st_size
 p=subprocess.run(['journalctl','-u','aetherpd-start@8-2580-78666.service','-n','60','--no-pager','-o','cat'],capture_output=True,text=True,timeout=5);report['session_journal']=p.stdout[-32768:];report['passed']=True
except Exception as e:report['error']=type(e).__name__+': '+str(e)
finally:
 if observer:observer.close()
 print(json.dumps(report),flush=True)
