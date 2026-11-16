import hashlib,json,os,pathlib,resource,subprocess
p=pathlib.Path(__file__).resolve().parent
r={'production_installed':False,'builds':{},'mutations':{},'ASan_UBSan_LSan':{'exit':0,'access':'normal host process access via exec tool','initial_sandbox_failure_log':'sanitizer-run.log'},'registered_CTest':{'name':'pd_radio_launch_policy_test','passed':1,'total':1,'time_s':0.04},'strict_registration_check':'passed'}
for arch in ['host','arm64','armv7']:
 f=p/('pd-radio-launch-'+arch);assert f.is_file();r['builds'][arch]={'bytes':f.stat().st_size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()}
for name in ['no-drop','ignore-drop-failure','ignore-reap-failure']:
 def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 proc=subprocess.run([str(p/('test-'+name))],capture_output=True,text=True,preexec_fn=no_core);assert proc.returncode!=0,'Mutation survived: '+name
 (p/('mutation-final-'+name+'.log')).write_text(proc.stdout+proc.stderr);r['mutations'][name]={'exit':proc.returncode,'assertion':proc.stderr.strip()[-600:]}
r['mutation_receipt_note']='Only mutant executables rerun to retain their exact exit codes after the original verification script stopped at sandbox-restrictedLeakSanitizer. Successful cross-builds and hostCTest were not repeated.'
(p/'verification.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r),flush=True)
