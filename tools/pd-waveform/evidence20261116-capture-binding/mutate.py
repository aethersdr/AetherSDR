from pathlib import Path
import subprocess,os,hashlib,json
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source')
out=Path(__file__).resolve().parent
build=out.parent/'first-install-desktop-monitor/project'
p=root/'src/core/txlinearity/AetherPDCaptureSource.cpp'
original=p.read_bytes()
needle=b'(m_binding && m_manifest->feedbackBinding != m_binding)'
assert original.count(needle)==1
env=dict(os.environ,CCACHE_DIR=str(out.parent/'first-install-desktop-monitor/ccache'),QT_QPA_PLATFORM='offscreen')
result={}
def run(name,command,check=True):
 with (out/name).open('w') as f:return subprocess.run(command,cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT,check=check)
try:
 p.write_bytes(original.replace(needle,b'false'))
 run('mutation-build.log',['cmake','--build',str(build),'--target','aetherpd_capture_source_test','--parallel','4'])
 r=run('mutation-test.log',[str(build/'aetherpd_capture_source_test')],False)
 result={'mutation':'omit snapshot binding comparison','exit':r.returncode,'expected_assertion': 'mismatched or unsupported binding retried or published samples' in (out/'mutation-test.log').read_text()}
 assert r.returncode!=0 and result['expected_assertion'],result
finally:
 p.write_bytes(original)
 result['restored_sha256']=hashlib.sha256(p.read_bytes()).hexdigest()
 run('restored-build.log',['cmake','--build',str(build),'--target','aetherpd_capture_source_test','aetherpd_dialog_test','tx_operation_integration_test','AetherSDR','--parallel','4'])
 r=run('restored-tests.log',['ctest','--test-dir',str(build),'--output-on-failure','--no-tests=error','-R','^(aetherpd_(protocol|client|model|dialog|capture_source)_test|tx_operation_integration_test|tx_linearity_controller_test|pd_capture_test|pd_calibration_dispatch_test)$'],False)
 result['restored_exit']=r.returncode
 (out/'mutation-result.json').write_text(json.dumps(result,indent=2)+'\n')
 assert r.returncode==0,result
print(json.dumps(result))
