import json,os,pathlib,shlex,subprocess
p=pathlib.Path(__file__).resolve().parent;w=p.parents[1]/'aetherpd-source'
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Core'],text=True));results={}
for name in ('flex_ptt_stop_tracker_test','flex_ptt_wire_session_test'):
 sources=[w/'tests'/f'{name}.cpp',w/'src/core/TxCoordinator.cpp',w/'src/core/backends/flex/FlexPttStopTracker.cpp']
 if 'wire_session' in name:sources.append(w/'src/core/backends/flex/FlexPttWireSession.cpp')
 cmd=['c++','-std=c++20','-O1','-g','-fPIC','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(w/'src'),*map(str,sources),*flags,'-o',str(p/(name+'-asan'))]
 with (p/(name+'-asan-build.log')).open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 env=os.environ.copy();env.update(ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
 with (p/(name+'-asan.log')).open('w') as log:q=subprocess.run([str(p/(name+'-asan'))],env=env,stdout=log,stderr=subprocess.STDOUT)
 results[name]={'returncode':q.returncode,'ASan':True,'UBSan':True,'LSan':False};assert q.returncode==0
(p/'sanitizers.json').write_text(json.dumps(results,indent=2));print(json.dumps(results))
