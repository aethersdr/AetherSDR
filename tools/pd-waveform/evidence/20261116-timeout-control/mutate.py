import os,subprocess,json
from pathlib import Path
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source')
out=Path(__file__).resolve().parent
build=out.parent/'first-install-desktop-monitor/project'
env=dict(os.environ,CCACHE_DIR=str(out.parent/'first-install-desktop-monitor/ccache'))
p=root/'src/core/txlinearity/TxLinearityController.cpp';original=p.read_text()
before='m_deadlineMs = now + std::min(m_config.timeoutMs, m_safety.limits().maxTransmitMs);'
assert original.count(before)==1
def compile_test(name):
 with (out/name).open('w') as log:
  subprocess.run(['cmake','--build',str(build),'--parallel','4','--target','tx_linearity_controller_test'],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
def run_test(name):
 with (out/name).open('w') as log:
  return subprocess.run(['ctest','--test-dir',str(build),'--output-on-failure','--no-tests=error','-R','^tx_linearity_controller_test$'],env=env,stdout=log,stderr=subprocess.STDOUT).returncode
try:
 p.write_text(original.replace(before,'m_deadlineMs = now + m_safety.limits().maxTransmitMs;'))
 compile_test('mutation-build.log');result=run_test('mutation-tests.log')
 assert result!=0 and 'selected timeout was not snapped into lease deadline' in (out/'mutation-tests.log').read_text()
 (out/'mutation-result.json').write_text(json.dumps({'ignored_selected_timeout_test_failed':True,'exit':result},indent=2)+'\n')
finally:
 p.write_text(original);compile_test('restored-build.log')
 assert run_test('restored-tests.log')==0
 assert p.read_text()==original
