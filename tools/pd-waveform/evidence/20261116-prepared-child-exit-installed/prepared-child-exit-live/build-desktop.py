from pathlib import Path
import hashlib,json,os,subprocess
B=Path(__file__).resolve().parent.parent;W=B.parent/'aetherpd-source';O=Path(__file__).resolve().parent;P=B/'first-install-desktop-monitor/project'
source=subprocess.check_output(['git','rev-parse','HEAD'],cwd=W,text=True).strip();assert source.startswith('ecbb9742b') and not subprocess.check_output(['git','status','--porcelain'],cwd=W).strip()
selection=B/'upgrade-prepared-child-exit/short/selection.json';manifest=json.loads((selection.parent/'manifest.json').read_text());assert manifest['native_release_verified'] and manifest['native_worker_verified']
cmds=[['cmake','-S',str(W),'-B',str(P),'-DAETHERPD_DEVELOPMENT_SELECTION='+str(selection)],['cmake','--build',str(P),'--target','AetherSDR','--parallel','4']]
env=dict(os.environ,CCACHE_DIR=str(B/'first-install-desktop-monitor/ccache'))
for i,cmd in enumerate(cmds):
 with (O/('desktop-build-'+str(i)+'.log')).open('w') as log:subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
f=P/'AetherSDR';r={'source_commit':source,'binary':str(f),'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'bytes':f.stat().st_size,'selection':str(selection),'selection_sha256':hashlib.sha256(selection.read_bytes()).hexdigest(),'commands':cmds};(O/'desktop-provenance.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r),flush=True)
