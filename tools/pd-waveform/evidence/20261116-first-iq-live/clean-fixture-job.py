"""Preserve the prior qualified fresh installation; provide no candidate payload or authorization."""
import hashlib,json,os,pathlib,shutil,socket,subprocess,urllib.request
P=pathlib.Path;parent=P('/var/lib/smartsdr');store=parent/'aetherpd'
backup=parent/'aetherpd-first-iq-backup-fdf0a40861ddbc0a';ram=P('/run/aetherpd-first-iq-backup-fdf0a40861ddbc0a')
units=P('/etc/systemd/system');names={'aetherpd-boot.service','aetherpd-shim.service','aetherpd-start.socket','aetherpd-start@.service'}
ns=['/usr/bin/nsenter','--target','1','--cgroup']
def command(args):
 p=subprocess.run(ns+args,capture_output=True,text=True,timeout=30)
 assert p.returncode==0,str(args)+': '+p.stderr[:300]
 return p.stdout
with urllib.request.urlopen('http://192.168.50.100:48992/v1/status',timeout=3) as r:status=json.load(r)
assert status['sessions']==0 and status['provisioned'] and status['state']=='local'
identity=hashlib.sha256((store/'state/provision.json').read_bytes()).hexdigest()
assert identity=='dac9c7673f8bf67b9d544471fa25410d19c1df0b016916d888fd7bcf2410c07d'
assert not backup.exists() and not ram.exists() and not (parent/'aetherpd-installation').exists()
containers=command(['/usr/bin/podman','ps','-a','--format','{{.Names}}']).splitlines()
assert not any(x in containers for x in ['flex-tailnet-shim','aetherpd-before-first-24cfa90ab5052bf8'])
for p in P('/proc').iterdir():
 if not p.name.isdecimal():continue
 try:
  exe=os.readlink(p/'exe');assert not exe.startswith('/run/aetherpd/') or exe=='/run/aetherpd/bin/flex-tailnet-shim',exe
 except (FileNotFoundError,ProcessLookupError,PermissionError):pass
entries=[]
for p in units.rglob('aetherpd*'):
 assert p.name in names and (p.is_file() or p.is_symlink()),str(p)
 entries.append(p.relative_to(units))
assert {x.name for x in entries}==names
backup.mkdir(mode=0o700)
manifest={'purpose':'recoverable selected-IQ first-install fixture; prior e0563d1e preserved','store':'aetherpd','units':[str(x) for x in entries],'original_provision_sha256':identity,'rf_commands':[]}
(backup/'manifest.json').write_text(json.dumps(manifest));(backup/'manifest.json').chmod(0o600)
# Copy units before stopping. The units and store may be on different filesystems.
for rel in entries:
 source=units/rel;target=backup/'units'/rel;target.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
 shutil.copy2(source,target,follow_symlinks=False)
 if source.is_symlink():assert target.is_symlink() and os.readlink(source)==os.readlink(target)
 else:
  assert source.read_bytes()==target.read_bytes() and source.stat().st_mode==target.stat().st_mode
  with target.open('rb') as f:os.fsync(f.fileno())
command(['/usr/bin/systemctl','stop','aetherpd-start.socket','aetherpd-shim.service','aetherpd-boot.service'])
for p in P('/proc').iterdir():
 if not p.name.isdecimal():continue
 try:assert not os.readlink(p/'exe').startswith('/run/aetherpd/')
 except (FileNotFoundError,ProcessLookupError,PermissionError):pass
for rel in entries:(units/rel).unlink()
store.rename(backup/'store')
if P('/run/aetherpd').exists():P('/run/aetherpd').rename(ram)
for p in [backup,backup/'units',parent,units]:
 fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
command(['/usr/bin/systemctl','daemon-reload'])
try:
 with socket.create_connection(('192.168.50.100',48992),1):raise AssertionError('old listener remains')
except ConnectionRefusedError:pass
assert not store.exists() and not P('/run/aetherpd').exists()
assert hashlib.sha256((backup/'store/state/provision.json').read_bytes()).hexdigest()==identity
print(json.dumps({'passed':True,'backup':str(backup),'runtime_backup':str(ram),'prior_identity_preserved':True,'installed_store_absent':True,'shim_listener_absent':True,'fixed_units_moved':manifest['units'],'rf_commands':[]}),flush=True)
