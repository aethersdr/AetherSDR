import json,pathlib,resource,shlex,subprocess
out=pathlib.Path(__file__).resolve().parent;w=out.parent.parent/'aetherpd-source';b=out.parent/'first-install-desktop-monitor/project'
lines=subprocess.check_output(['ninja','-C',str(b),'-t','commands','pd_capture_test'],text=True).splitlines()
entry=w/'tools/pd-waveform/capture_test.c';source=w/'tools/pd-waveform/pd_waveform.c'
def compile(source_path,new_source,obj):
 args=next(shlex.split(l) for l in lines if ' -c '+str(source_path) in l)
 if args[0]=='ccache':args=args[1:]
 old=args[args.index('-o')+1];clean=[];i=0
 while i<len(args):
  x=args[i]
  if x in ['-MF','-MT']:i+=2;continue
  if x=='-MD':i+=1;continue
  clean.append(str(new_source) if x==str(source_path) else str(obj) if x==old else x);i+=1
 subprocess.run(clean,cwd=b,check=True);return old
results={}
for name,before,after in [('tap','pd_capture_batch_feed(&w->capture_batch, &meta, v.payload, (size_t)v.words * 4, now);','(void)now;'),('cancel','log_bounded_source_end(w, reason);\n    pd_capture_batch_invalidate(&w->capture_batch);','log_bounded_source_end(w, reason);')]:
 text=source.read_text();assert text.count(before)==1
 mutant=out/(name+'-mutant.c');mutant.write_text(text.replace(before,after))
 test=out/(name+'-test.c');test.write_text(entry.read_text().replace('#include "pd_waveform.c"','#include "'+str(mutant)+'"'))
 obj=out/(name+'.o');old=compile(entry,test,obj)
 args=[str(obj) if x==old else x for x in shlex.split(lines[-1])[2:-2] if not x.startswith('-Wl,--dependency-file=')]
 binary=out/(name+'-mutant');args[args.index('-o')+1]=str(binary);subprocess.run(args,cwd=b,check=True)
 def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 r=subprocess.run([str(binary)],capture_output=True,text=True,preexec_fn=no_core)
 (out/(name+'-mutation.log')).write_text(r.stdout+r.stderr);assert r.returncode!=0 and 'Assertion' in r.stderr
 results[name]={'exit':r.returncode,'assertion':r.stderr.splitlines()[-1]}
source=w/'tools/pd-waveform/pd_capture.c';text=source.read_text();before='samples += take; bytes -= take;';assert text.count(before)==1
mutant=out/'tail-mutant.c';mutant.write_text(text.replace(before,'samples += take; bytes = 0;'))
obj=out/'tail.o';compile(source,mutant,obj)
args=[]
for x in shlex.split(lines[-1])[2:-2]:
 if x.startswith('-Wl,--dependency-file='):continue
 if x=='libaether_pd_test_core.a':args.append(str(obj))
 args.append(x)
binary=out/'tail-mutant';args[args.index('-o')+1]=str(binary);subprocess.run(args,cwd=b,check=True)
r=subprocess.run([str(binary)],capture_output=True,text=True,preexec_fn=no_core)
(out/'tail-mutation.log').write_text(r.stdout+r.stderr);assert r.returncode!=0 and 'Assertion' in r.stderr
results['packet_tail']={'exit':r.returncode,'assertion':r.stderr.splitlines()[-1]}
(out/'mutations.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results))
