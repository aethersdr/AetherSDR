from pathlib import Path
import subprocess,json,resource,hashlib
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source/tools/pd-waveform/session')
out=Path(__file__).resolve().parent
cases=[('selected-pan','pd_radio_launch.c',' || (selection.pan && selection.pan!=feedback)','', 'radio_launch_test.c','!radio_stream_ready(altered,&sid,&pan,selection)'),
       ('selected-channel','pd_start_session.c','start_u32(frame+48),start_u32(frame+52)','1,start_u32(frame+52)', 'start_session_test.c','selection.channel==channel')]
results=[]
for name,file,old,new,test,expected in cases:
 p=root/file;original=p.read_text();assert original.count(old)==1
 binary=out/(name+'-mutation')
 command=['cc','-O1','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',str(root/test),'-o',str(binary)]
 result={'mutation':name}
 try:
  p.write_text(original.replace(old,new));subprocess.run(command,check=True)
  r=subprocess.run([str(binary)],capture_output=True,text=True)
  (out/(name+'-mutation.log')).write_text(r.stdout+r.stderr)
  result.update(exit=r.returncode,expected_assertion_failed=expected in r.stderr)
  assert r.returncode and result['expected_assertion_failed'],r.stderr
 finally:
  p.write_text(original);result['restored_sha256']=hashlib.sha256(p.read_bytes()).hexdigest()
  subprocess.run(command,check=True)
  r=subprocess.run([str(binary)],capture_output=True,text=True)
  (out/(name+'-restored.log')).write_text(r.stdout+r.stderr)
  result['restored_exit']=r.returncode;results.append(result)
  (out/'mutations.json').write_text(json.dumps(results,indent=2)+'\n')
  assert not r.returncode,r.stderr
print(results)
