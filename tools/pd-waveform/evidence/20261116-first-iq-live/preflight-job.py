"""Read-only installed-state audit before any fresh-install fixture changes."""
import hashlib,json,os,pathlib,subprocess
P=pathlib.Path;store=P('/var/lib/smartsdr/aetherpd');report={'rf_commands':[],'installation_mutated':False}
identity=(store/'state/provision.json').read_bytes();cfg=json.loads(identity)
report['identity_sha256']=hashlib.sha256(identity).hexdigest();report['installation_id']=cfg['installation_id'];report['local_only']=cfg['local_only']
assert report['identity_sha256']=='dac9c7673f8bf67b9d544471fa25410d19c1df0b016916d888fd7bcf2410c07d'
assert report['installation_id']=='421daf94e1ebbd8502975ad35e9b47022548bff1883a17506db0eb32be5dc78c' and cfg['local_only']
report['boot_id']=P('/proc/sys/kernel/random/boot_id').read_text().strip();report['uptime']=P('/proc/uptime').read_text().strip()
generation=(store/'upgrade-generation').read_bytes();assert generation==b'PDGEN001'+bytes.fromhex('c1346b1bc5279292483d0be04cb38ee572a64ce66896194a58a473e72eea1c46');report['generation']=generation[8:].hex()
report['processes']=[]
for p in P('/proc').iterdir():
 if not p.name.isdecimal():continue
 try:
  exe=os.readlink(p/'exe')
  if not exe.startswith('/run/aetherpd/'):continue
  s={k:v.strip() for k,v in (line.split(':',1) for line in (p/'status').read_text().splitlines() if ':' in line)}
  report['processes'].append({'pid':int(p.name),'exe':exe,'uid':s['Uid'],'nnp':s['NoNewPrivs'],'caps':{k:s[k] for k in ['CapInh','CapPrm','CapEff','CapBnd','CapAmb']},'sha256':hashlib.sha256((p/'exe').read_bytes()).hexdigest()})
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
assert len(report['processes'])==1 and P(report['processes'][0]['exe']).name=='flex-tailnet-shim'
p=report['processes'][0];assert p['uid'].split()==['78666']*4 and p['nnp']=='1' and all(int(v,16)==0 for v in p['caps'].values())
assert p['sha256']=='5b6371126432a236f079c794d8c0277673ef2fdfaae7d59e54cea33aea7632e0'
report['arp']=[x for x in P('/proc/net/arp').read_text().splitlines()[1:] if x.split()[0]=='172.30.2.1'];assert not report['arp']
report['accept_local']=P('/proc/sys/net/ipv4/conf/eth0.10/accept_local').read_text().strip();assert report['accept_local']=='0'
report['temporary_paths']={str(p):p.exists() for p in [store.parent/n for n in ['aetherpd-upgrade','.aetherpd-upgrade','.aetherpd-upgrade-retired','.aetherpd-upgrade-staging','aetherpd-installation']]+[P('/etc/systemd/system')/n for n in ['aetherpd-upgrade.service','aetherpd-upgrade-recover.service','aetherpd-install.service','aetherpd-recover.service']]};assert not any(report['temporary_paths'].values())
ns=['/usr/bin/nsenter','--target','1','--cgroup']
for name,args in [('services',['/usr/bin/systemctl','show','aetherpd-boot.service','aetherpd-shim.service','aetherpd-start.socket','--property=Id,ActiveState,SubState,Result,ExecMainStartTimestampMonotonic']),('containers',['/usr/bin/podman','ps','-a','--format','{{.Names}}']),('boot_journal',['/usr/bin/journalctl','-b','-u','aetherpd-boot.service','-u','aetherpd-shim.service','--no-pager','-o','cat','-n','60'])]:
 r=subprocess.run(ns+args,capture_output=True,text=True,timeout=15);assert r.returncode==0;rtext=r.stdout
 # Fixed service journal only; never print provision contents, environment or bearer.
 report[name]=rtext
report['passed']=True;print(json.dumps(report),flush=True)
