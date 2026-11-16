import os,subprocess,json
from pathlib import Path
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source')
out=Path(__file__).resolve().parent
build=out.parent/'first-install-desktop-monitor/project'
env=dict(os.environ,QT_QPA_PLATFORM='offscreen',CCACHE_DIR=str(out.parent/'first-install-desktop-monitor/ccache'))
mutations={
 root/'src/core/txlinearity/TxLinearityController.cpp':(' && limits.maxTransmitMs <= kMaxTransmitMs',''),
 root/'src/models/AetherPDRunLease.cpp':('constexpr qint64 kMaximumRunMs = txlin::TxLinearityController::kMaxTransmitMs;','constexpr qint64 kMaximumRunMs = 15000;')}
original={p:p.read_text() for p in mutations}
try:
 for p,(before,after) in mutations.items():
  assert original[p].count(before)==1
  p.write_text(original[p].replace(before,after))
 with (out/'mutation-build.log').open('w') as log:
  subprocess.run(['cmake','--build',str(build),'--parallel','4','--target','tx_linearity_controller_test','tx_operation_integration_test'],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 with (out/'mutation-tests.log').open('w') as log:
  result=subprocess.run(['ctest','--test-dir',str(build),'--output-on-failure','--no-tests=error','-R','^(tx_linearity_controller_test|tx_operation_integration_test)$'],env=env,stdout=log,stderr=subprocess.STDOUT)
 text=(out/'mutation-tests.log').read_text()
 assert result.returncode!=0 and 'even telemetry override cannot extend analyzer transmit ceiling' in text and 'insufficient startup budget or ceiling violation refuses' in text
 (out/'mutation-result.json').write_text(json.dumps({'both_guards_removed_tests_failed':True,'ctest_exit':result.returncode,'radio_used':False},indent=2)+'\n')
finally:
 for p,text in original.items():p.write_text(text)
 with (out/'restored-build.log').open('w') as log:
  subprocess.run(['cmake','--build',str(build),'--parallel','4','--target','AetherSDR','tx_linearity_controller_test','tx_operation_integration_test','aetherpd_capture_source_test','aetherpd_model_test','aetherpd_dialog_test'],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
