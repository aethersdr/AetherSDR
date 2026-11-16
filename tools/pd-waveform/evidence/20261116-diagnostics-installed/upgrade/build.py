"""Build a complete development upgrade distribution; never upload or key."""
import hashlib, importlib.util, json, os, pathlib, shutil, subprocess, sys, time
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
P=pathlib.Path
OUT=P(__file__).resolve().parent
PRIOR=OUT.parent/'runtime-retained-diagnostics'
GO=P('/home/jeremy/go/pkg/mod/golang.org/toolchain@v0.0.1-go1.27.1.linux-amd64/bin/go')
def sha(data):return hashlib.sha256(data).hexdigest()
def child(label):
 settings=json.load(sys.stdin);seed=bytes.fromhex(settings['seed']);key=Ed25519PrivateKey.from_private_bytes(seed);public=key.public_key().public_bytes_raw()
 prior=PRIOR/label;root=prior/'source';out=OUT/label;out.mkdir(exist_ok=False);package=out/'package';package.mkdir()
 sys.path.insert(0,str(root/'tools/pd-waveform/boot'))
 import build as boot,bundle,release,units,bootstrap_artifact
 manifest=json.loads((prior/'arm64/manifest.json').read_text());source=bytes.fromhex(manifest['payload']['source_sha256'])
 raw=(prior/'arm64/candidate.release').read_bytes()
 assert sha(raw)==manifest['release']['sha256'] and manifest['release']['public_key']==public.hex()
 (package/'candidate.release').write_bytes(raw);(out/'public-key').write_bytes(public);(out/'release-digest').write_bytes(bundle.blake(raw));(out/'source-digest').write_bytes(source)
 previous=(OUT.parent/'first-guardian-owner-ready/short/package/candidate.release').read_bytes()
 assert sha(previous)=='3e4c1ce394e7c5909e4670be2edea330d23875a67b71dba7239869ec1d89dee7'
 (package/'previous.release').write_bytes(previous)
 worker=boot.build(out/'worker','arm64','pd_upgrade_worker.c',public,management_release=package/'candidate.release')
 (package/'worker.envelope').write_bytes(bootstrap_artifact.assemble(worker.read_bytes(),64,2,source,bundle.blake(raw),seed))
 for entry in ('pd_accept.c','pd_send.c','pd_deliver.c'):
  binary=boot.build(out/entry.removesuffix('.c'),'arm64',entry,public);shutil.copyfile(binary,package/binary.name)
 env=dict(os.environ,CGO_ENABLED='0',GOOS='linux',GOARCH='arm64',GOTOOLCHAIN='local',GOPROXY='off',GOCACHE=str(out/'go-cache'))
 env.pop('GOARM',None)
 subprocess.run([str(GO),'build','-trimpath','-buildvcs=false','-ldflags=-s -w','-o',str(package/'pd-carrier'),'.'],cwd=root/'tools/pd-waveform/carrier',env=env,check=True)
 (package/'LICENSES').write_bytes(b'\n'.join([(root/'runtime-LICENSES').read_bytes(),(root/'LICENSE').read_bytes(),(root/'tools/pd-waveform/boot/BOOT-LICENSES.txt').read_bytes(),(GO.parent.parent/'LICENSE').read_bytes()]))
 verify=boot.build(out/'verify','host','release_check.c')
 subprocess.run([str(verify),str(package/'candidate.release'),str(out/'public-key'),'64','1','4.3.16.42034',str(out/'release-digest')],check=True)
 verify=boot.build(out/'verify','host','bootstrap_artifact_check.c')
 subprocess.run([str(verify),str(package/'worker.envelope'),str(out/'public-key'),'64','2',str(out/'source-digest'),str(out/'release-digest')],check=True)
 pins={p.name:{'size':p.stat().st_size,'sha256':sha(p.read_bytes())} for p in sorted(package.iterdir())}
 selection={'version':'0.0.0-diagnostics.'+settings['commit'][:9],'firmware':'4.3.16.42034','profile':1,'candidate_key':public.hex(),'previous_key':public.hex(),'files':pins}
 (out/'selection.json').write_text(json.dumps(selection,indent=2)+'\n')
 subprocess.run([sys.executable,str(root/'tools/pd-waveform/development_selection.py'),str(out/'selection.json'),str(out/'upgrade-selection.h')],check=True)
 (out/'manifest.json').write_text(json.dumps({'selection':selection,'release':manifest['release'],'payload':manifest['payload'],'previous_release_sha256':sha(previous),'worker_bytes':worker.stat().st_size,'native_release_verified':True,'native_worker_verified':True},indent=2)+'\n')
 print('Complete '+label,flush=True)
def main():
 prior=json.loads((PRIOR/'report.json').read_text());seed_result=subprocess.run([str(OUT.parent/'development-publisher/vault'),'read'],capture_output=True,timeout=20,check=True);key=Ed25519PrivateKey.from_private_bytes(bytes.fromhex(seed_result.stdout.decode()));assert key.public_key().public_bytes_raw().hex()=='9a4286d5d3e006c5237a2fa3cf282ec3b9710a4e1b92b18d80f0122177e5a7ce';settings=json.dumps({'seed':key.private_bytes_raw().hex(),'commit':prior['source_commit']});start=time.monotonic()
 for label in ('short','second-distinct-long-source-path'):
  with (OUT/(label+'.log')).open('w') as log:
   subprocess.run([sys.executable,str(P(__file__).resolve()),'--child',label],input=settings,text=True,stdout=log,stderr=subprocess.STDOUT,check=True)
  print('Completed '+label,flush=True)
 a,b=[OUT/n for n in ('short','second-distinct-long-source-path')];ma,mb=[json.loads((p/'manifest.json').read_text()) for p in (a,b)]
 assert ma==mb
 assert all((a/'package'/n).read_bytes()==(b/'package'/n).read_bytes() for n in ma['selection']['files'])
 report={'source_commit':prior['source_commit'],'runtime_source_manifest_sha256':prior['source_manifest_sha256'],'script_sha256':sha(P(__file__).read_bytes()),'complete_eight_file_distribution_identical':True,'separate_source_output_and_compiler_caches':True,'same_host_and_toolchains':True,'payloads_reused_from_verified_two_path_runtime_build':True,'private_seed_in_files':False,'development_signer':'QtKeychain AetherPD development publisher / flex8600-bench-20261116','installed':False,'production_ready':False,'elapsed_seconds':time.monotonic()-start,'first':ma,'limitations':['ARM64 FLEX-8600 upgrade distribution only; no FLEX-6000 first-install adapter.','Development publisher key; not a shipping catalogue or deployed-key rotation authorization.','Desktop preparation, installed recovery and RF qualification are separate gates.']}
 (OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n');print('All eight upgrade distribution files reproduced.',flush=True)
if __name__=='__main__':
 if len(sys.argv)>1:child(sys.argv[2])
 else:main()
