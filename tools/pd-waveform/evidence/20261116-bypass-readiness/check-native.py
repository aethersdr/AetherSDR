import os, sys, shlex, subprocess, json, concurrent.futures
from pathlib import Path
out=Path(__file__).resolve().parent
project=out.parent/'first-install-desktop-monitor/project'
flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-O1']
targets=['pd_dispatch_deadline_test','pd_calibration_dispatch_test']
commands={t:subprocess.check_output(['ninja','-C',str(project),'-t','commands',t],text=True).splitlines() for t in targets}
compile_args={}
for lines in commands.values():
 for line in lines:
  args=shlex.split(line)
  if '-c' not in args or '-o' not in args: continue
  if args[0]=='ccache': args=args[1:]
  old=args[args.index('-o')+1]
  new=str(out/('asan-'+Path(old).name))
  clean=[];i=0
  while i<len(args):
   if args[i] in ['-MF','-MT']: i+=2;continue
   if args[i]=='-MD':i+=1;continue
   clean.append(new if args[i]==old else args[i]);i+=1
  compile_args[old]=(new,clean+flags)
def compile_one(pair):
 subprocess.run(pair[1],cwd=project,check=True,stdout=subprocess.DEVNULL)
if '--reuse-objects' not in sys.argv:
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
  list(pool.map(compile_one,compile_args.values()))
core=[new for old,(new,args) in compile_args.items() if '/aether_pd_test_core.dir/' in old]
results={}
for target,lines in commands.items():
 args=shlex.split(lines[-1]);assert args[:2]==[':','&&'] and args[-2:]==['&&',':']
 link=[]
 for arg in args[2:-2]:
  if arg.startswith('-Wl,--dependency-file='):continue
  if arg=='libaether_pd_test_core.a':link.extend(o for o in core if not (target=='pd_dispatch_deadline_test' and o.endswith('/asan-pd_local_iq.c.o')))
  else:link.append(compile_args.get(arg,(arg,))[0])
 binary=out/(target+'-asan');link[link.index('-o')+1]=str(binary)
 subprocess.run(link+flags,cwd=project,check=True)
 run=subprocess.run([str(binary)],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
 (out/(target+'-asan.log')).write_text(run.stdout+run.stderr)
 results[target]=run.returncode
 if run.returncode:raise SystemExit(f'{target} failed: {run.stderr[-2000:]}')
(out/'native-sanitizers.json').write_text(json.dumps(results,indent=2)+'\n')
print(results)
