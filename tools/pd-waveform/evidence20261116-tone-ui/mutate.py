from pathlib import Path
import subprocess,os,json,resource,hashlib
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source');out=root.parent/'aetherpd-shim-integration/tone-ui';build=out.parent/'first-install-desktop-monitor/project'
env=dict(os.environ,CCACHE_DIR=str(out.parent/'first-install-desktop-monitor/ccache'))
cases=[('configured-spacing',root/'src/models/AetherPDMeasurementModel.cpp','? std::optional<quint32>{spacingHz} : std::nullopt','? std::optional<quint32>{aetherpd::kDefaultToneSpacingHz} : std::nullopt','aetherpd_dialog_test','explicit arm sends exact configured spacing through original-session route')]
results=[]
for name,path,old,new,target,assertion in cases:
 original=path.read_bytes();text=original.decode();assert text.count(old)==1
 try:
  path.write_text(text.replace(old,new))
  with (out/(name+'-mutation-build.log')).open('w') as log:
   subprocess.run(['cmake','--build',str(build),'--target',target,'--parallel','4'],env=env,check=True,stdout=log,stderr=subprocess.STDOUT)
  run=subprocess.run([str(build/target)],capture_output=True,text=True,env=dict(os.environ,QT_QPA_PLATFORM='offscreen'))
  (out/(name+'-mutation.log')).write_text(run.stdout+run.stderr)
  assert run.returncode!=0 and assertion in run.stdout+run.stderr,(name,run.returncode,run.stdout,run.stderr)
  results.append({'mutation':name,'caught':True,'exit':run.returncode,'assertion':assertion,'restored_source_sha256':hashlib.sha256(original).hexdigest()})
 finally:
  path.write_bytes(original)
  assert path.read_bytes()==original
 with (out/(name+'-restored-build.log')).open('w') as log:
  subprocess.run(['cmake','--build',str(build),'--target',target,'--parallel','4'],env=env,check=True,stdout=log,stderr=subprocess.STDOUT)
 run=subprocess.run([str(build/target)],capture_output=True,text=True,env=dict(os.environ,QT_QPA_PLATFORM='offscreen'))
 (out/(name+'-restored.log')).write_text(run.stdout+run.stderr);assert run.returncode==0
(out/'mutations.json').write_text(json.dumps(results,indent=2)+'\n');print(results)
