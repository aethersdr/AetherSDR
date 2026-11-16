import hashlib,json,os,pathlib,subprocess
P=pathlib.Path;store=P('/var/lib/smartsdr/aetherpd');report={'rf_commands':[],'processes':[]}
try:
 plan=(store/'migration-approved').read_bytes();assert hashlib.blake2b(plan,digest_size=32).hexdigest()=='cc0929ab2bd881ac33b45a812be2d0f37230c779815150c1608fb41d007fd30d'
 assert (store/'transaction').read_bytes()==b'PDTXN001'+plan+bytes([0,0,0,5])
 cfg=json.loads((store/'state/provision.json').read_text());assert cfg['local_only'] and len(cfg['installation_id'])==64 and cfg['installation_id'].startswith('b6050e0036d2cfe9')
 assert not (store/'state/tsnet').exists()
 report['installation_id']=cfg['installation_id']
 report['identity_sha256']=hashlib.sha256((store/'state/provision.json').read_bytes()).hexdigest()
 report['temporary_paths']={p:P(p).exists() for p in ['/var/lib/smartsdr/aetherpd-installation','/run/aetherpd-install','/run/aetherpd-delivery','/etc/systemd/system/aetherpd-install.service','/etc/systemd/system/aetherpd-recover.service','/var/lib/smartsdr/aetherpd-upgrade','/etc/systemd/system/aetherpd-upgrade.service','/etc/systemd/system/aetherpd-upgrade-recover.service']}
 assert not any(report['temporary_paths'].values())
 for p in P('/proc').iterdir():
  if not p.name.isdecimal():continue
  try:
   exe=os.readlink(p/'exe')
   if exe.startswith(('/run/aetherpd/', '/var/lib/smartsdr/aetherpd/')) or 'pd-first-request' in exe:
    status={l.split(':',1)[0]:l.split(':',1)[1].strip() for l in (p/'status').read_text().splitlines() if ':' in l}
    row={'pid':int(p.name),'exe':exe,'uid':status['Uid'],'caps':{k:status[k] for k in ['CapInh','CapPrm','CapEff','CapBnd','CapAmb']},'nnp':status['NoNewPrivs'],'sha256':hashlib.sha256((p/'exe').read_bytes()).hexdigest()};report['processes'].append(row)
  except (FileNotFoundError,ProcessLookupError,PermissionError):pass
 assert len(report['processes']) in (1,3)
 assert all(x['uid'].split()==['78666']*4 and all(int(v,16)==0 for v in x['caps'].values()) and x['nnp']=='1' for x in report['processes'])
 assert not any(P(x['exe']).name in ['aether-pd','pd-iq-source','pd-capture-bootstrap'] for x in report['processes'])
 primary=next(x for x in report['processes'] if x['exe']=='/run/aetherpd/bin/flex-tailnet-shim');assert primary['exe']=='/run/aetherpd/bin/flex-tailnet-shim' and primary['uid'].split()==['78666']*4 and all(int(x,16)==0 for x in primary['caps'].values()) and primary['nnp']=='1'
 assert primary['sha256']=='4542c96d1d7c92e1c0c6939d446432461b0702a6ad6fe511a8c5d0501288b2fa'
 report['private_sockets']=[l for l in P('/proc/net/unix').read_text().splitlines() if any(x in l for x in ['aetherpd-config-','aetherpd-bootstrap-','aetherpd-relay-'])];assert not report['private_sockets']
 ns=['/usr/bin/nsenter','--target','1','--cgroup']
 p=subprocess.run(ns+['/usr/bin/podman','ps','-a','--format','{{.Names}}'],capture_output=True,text=True,timeout=10);assert p.returncode==0
 report['containers']=p.stdout.splitlines();assert not any(x.startswith(('aetherpd-first-','aetherpd-install-','aetherpd-carrier-')) for x in report['containers'])
 report['arp']=[l for l in P('/proc/net/arp').read_text().splitlines()[1:] if l.split()[0]=='172.30.2.1'];assert not report['arp']
 report['accept_local']=P('/proc/sys/net/ipv4/conf/eth0.10/accept_local').read_text().strip();assert report['accept_local']=='0'
 report['legacy_container_absent']='aetherpd-before-first-24cfa90ab5052bf8' not in report['containers'];assert report['legacy_container_absent']
 report['cached_first_artifacts']=[]
 for root in ['/user/waveforms','/var/lib/smartsdr']:
  for item in P(root).rglob('*b6050e0036d2cfe9*'):
   report['cached_first_artifacts'].append(str(item))
 assert not report['cached_first_artifacts']
 expected={'flex-tailnet-shim': '4542c96d1d7c92e1c0c6939d446432461b0702a6ad6fe511a8c5d0501288b2fa', 'aether-pd': 'c28641355da9dfb2fee2969d8a29b15e8a44362c04abf9aa649890b79c299151', 'pd-primary-launch': '21050e15565de8295c8bd1a47b4072bbc0a7900a4fd8f01c9b1a744e197164bc', 'pd-start-session': '7772765fd596722dc342f2579c83f3a3186614979c9defbc141223f7ae18c525', 'pd-radio-launch': '4ca6468b3eb9f023937170fdd0bac1c4c38bf017d29e8f3310b94010596cc1d5', 'pd-fixed-arp': '2e6001780d2c8d42f2d422237cea7a4dcbc24d1ace9ee4aff7ee422a09072f49', 'pd-iq-source': '37fa974279793ccb0b4ee5891bc361a536d1cc917a12521ffe8d09781cc35ac0', 'pd-capture-bootstrap': 'b3132499052fdb709c64fa0267171072299aae21fe0a970c0debc4648e40eccc', 'pd-unkey-guard': 'd945aeaf3926c51e63a2054d64f4bd799334d453cc575e060ec37464cadf7662', 'pd-lease-guard': '297183f7504ffd6c9c6d9f4713976a2bcbfc1df5d9caa788c402fd8568c0954e'}
 report['runtime_hashes']={n:hashlib.sha256((P('/run/aetherpd/bin')/n).read_bytes()).hexdigest() for n in expected if n!='LICENSES'}
 assert all(report['runtime_hashes'][n]==h for n,h in expected.items() if n!='LICENSES')
 assert (store/'upgrade-generation').is_file()
 report['upgrade_generation_present']=True
 assert report['identity_sha256']=='f511e5f09f41a99b0f5cd29e4e321eb6f93ec02162df41175ae6866173233cb5'
 backup=P('/var/lib/smartsdr/aetherpd-guardian-backup-54e21aa67/store/state/provision.json')
 report['prior_backup_identity_sha256']=hashlib.sha256(backup.read_bytes()).hexdigest()
 assert report['prior_backup_identity_sha256']=='7e315aee252647fbbaf713d7633ec4b72fbd9c73406bedc54b107bedc5476931'
 report['boot_id']=P('/proc/sys/kernel/random/boot_id').read_text().strip();report['passed']=True
finally:
 print(json.dumps(report),flush=True)
