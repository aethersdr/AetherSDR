import shlex,subprocess,concurrent.futures,os,json
from pathlib import Path
out=Path(__file__).resolve().parent;build=out.parent/'first-install-desktop-monitor/project'
targets=['aetherpd_protocol_test','aetherpd_capture_source_test','tx_linearity_controller_test']
lines={t:subprocess.check_output(['ninja','-C',str(build),'-t','commands',t],text=True).splitlines() for t in targets}
objects={};flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-O1']
for commands in lines.values():
 for command in commands:
  args=shlex.split(command)
  if '-c' not in args or '-o' not in args:continue
  if args[0]=='ccache':args=args[1:]
  old=args[args.index('-o')+1];new=str(out/('asan-'+old.replace('/','_')))
  cleaned=[];i=0
  while i<len(args):
   if args[i] in ['-MF','-MT']:i+=2;continue
   if args[i]=='-MD':i+=1;continue
   cleaned.append(new if args[i]==old else args[i]);i+=1
  objects[old]=(new,cleaned+flags)
def compile_one(v):subprocess.run(v[1],cwd=build,stdout=subprocess.DEVNULL,check=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(compile_one,objects.values()))
results={}
for target,commands in lines.items():
 args=shlex.split(commands[-1]);assert args[:2]==[':','&&'] and args[-2:]==['&&',':']
 link=[objects.get(arg,(arg,None))[0] for arg in args[2:-2] if not arg.startswith('-Wl,--dependency-file=')]
 exe=out/(target+'-asan');link[link.index('-o')+1]=str(exe)
 subprocess.run(link+flags,cwd=build,check=True)
 run=subprocess.run([str(exe)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1'),capture_output=True,text=True)
 (out/(target+'-asan.log')).write_text(run.stdout+run.stderr);results[target]=run.returncode
 assert run.returncode==0,(target,run.stderr[-2000:])
(out/'sanitizers.json').write_text(json.dumps(results,indent=2)+'\n');print(results)
