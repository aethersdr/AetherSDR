import json,os,pathlib,subprocess
P=pathlib.Path;report={'read_only':True,'rf_commands':[],'processes':[]}
for p in P('/proc').iterdir():
 if not p.name.isdecimal():continue
 try:
  exe=os.readlink(p/'exe')
  if not exe.startswith('/run/aetherpd/') and 'pd-first-request' not in exe:continue
  status={k:v.strip() for k,v in (s.split(':',1) for s in (p/'status').read_text().splitlines() if ':' in s)}
  report['processes'].append({'pid':int(p.name),'exe':exe,'uid':status['Uid'],'ppid':status['PPid'],'state':status['State'],'nnp':status['NoNewPrivs'],'caps':{k:status[k] for k in ['CapInh','CapPrm','CapEff','CapBnd','CapAmb']}})
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
report['arp']=[s for s in P('/proc/net/arp').read_text().splitlines()[1:] if s.split()[0]=='172.30.2.1']
report['arp_journal_bytes']=P('/run/.aetherpd-fixed-arp.8600').stat().st_size
report['sockets']=[s for s in P('/proc/net/unix').read_text().splitlines() if 'aetherpd' in s]
r=subprocess.run(['/usr/bin/nsenter','--target','1','--cgroup','/usr/bin/journalctl','-b','-u','aetherpd-start@*','-n','80','--no-pager','-o','cat'],capture_output=True,text=True,timeout=15);report['journal_exit']=r.returncode;report['journal']=r.stdout
print(json.dumps(report),flush=True)
