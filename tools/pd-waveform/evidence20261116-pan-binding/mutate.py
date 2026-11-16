from pathlib import Path
import subprocess,os,json,hashlib,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source')
out=Path(__file__).resolve().parent
build=out.parent/'first-install-desktop-monitor/project'
env=dict(os.environ,CCACHE_DIR=str(build.parent/'ccache'))
cases=[('pan-owner','session/pd_iq_stream.c','pan->owner==s->gui && ','', 'pd_iq_stream_policy_test','!wire(&s,rebind[i]) && s.failed'),
       ('dsp-binding','pd_waveform.c','return !w->sealed_feedback_pan || (','return 1 || (', 'pd_calibration_dispatch_test','!feedback_binding_valid(&w)')]
results=[]
def run(args,name):
 with (out/name).open('w') as log:
  return subprocess.run(args,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT).returncode
for name,file,old,new,target,assertion in cases:
 path=root/'tools/pd-waveform'/file;original=path.read_text();assert original.count(old)==1
 result={'mutation':name}
 command=['cmake','--build',str(build),'--target',target,'-j','4']
 try:
  path.write_text(original.replace(old,new))
  assert run(command,name+'-build.log')==0
  code=run(['ctest','--test-dir',str(build),'-R','^'+target+'$','--output-on-failure'],name+'-test.log')
  result.update(exit_code=code,expected_assertion_failed=assertion in (out/(name+'-test.log')).read_text())
  assert code and result['expected_assertion_failed']
 finally:
  path.write_text(original)
  result['restored_source_sha256']=hashlib.sha256(path.read_bytes()).hexdigest()
  result['restore_build_exit_code']=run(command,name+'-restore-build.log')
  results.append(result);(out/'mutations.json').write_text(json.dumps(results,indent=2)+'\n')
  assert result['restore_build_exit_code']==0
