from pathlib import Path
import subprocess,os,json
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source')
out=Path(__file__).resolve().parent
results={}
for name in ['radio_launch','start_session']:
 source=root/'tools/pd-waveform/session'/f'{name}_test.c'
 binary=out/(name+'-asan')
 command=['cc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',str(source),'-o',str(binary)]
 subprocess.run(command,check=True)
 run=subprocess.run([str(binary)],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
 (out/(name+'-asan.log')).write_text(run.stdout+run.stderr)
 results[name]=run.returncode
 assert not run.returncode,run.stderr
(out/'constructor-sanitizers.json').write_text(json.dumps(results,indent=2)+'\n')
print(results)
