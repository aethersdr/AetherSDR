import json, shlex, subprocess, resource
from pathlib import Path
out=Path(__file__).resolve().parent
project=out.parent/'first-install-desktop-monitor/project'
w=out.parent.parent/'aetherpd-source'
source=w/'tools/pd-waveform/pd_waveform.c'
results={}
for name,target,before,after in [
 ('alignment','pd_dispatch_deadline_test','if (evidence->line[0]) {','if (0) {'),
 ('source_end','pd_calibration_dispatch_test','++w->source_end_event.count;','(void)w;')]:
 text=source.read_text();assert text.count(before)==1
 mutant=out/(name+'-mutant.c');mutant.write_text(text.replace(before,after))
 entry=w/('tools/pd-waveform/'+target.removeprefix('pd_')+'.c')
 copied=out/(name+'-test.c');copied.write_text(entry.read_text().replace('#include "pd_waveform.c"','#include "'+str(mutant)+'"'))
 lines=subprocess.check_output(['ninja','-C',str(project),'-t','commands',target],text=True).splitlines()
 compile=next(shlex.split(line) for line in lines if ' -c '+str(entry) in line)
 if compile[0]=='ccache':compile=compile[1:]
 original=compile[compile.index('-o')+1];obj=str(out/(name+'.o'))
 args=[];i=0
 while i<len(compile):
  arg=compile[i]
  if arg in ('-MF','-MT'):i+=2;continue
  if arg=='-MD':i+=1;continue
  args.append(obj if arg==original else str(copied) if arg==str(entry) else arg);i+=1
 subprocess.run(args,cwd=project,check=True)
 args=shlex.split(lines[-1])[2:-2]
 args=[obj if a==original else a for a in args if not a.startswith('-Wl,--dependency-file=')]
 binary=out/(name+'-mutant');args[args.index('-o')+1]=str(binary)
 subprocess.run(args,cwd=project,check=True)
 def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 run=subprocess.run([str(binary)],capture_output=True,text=True,preexec_fn=no_core)
 (out/(name+'-mutation.log')).write_text(run.stdout+run.stderr)
 assert run.returncode!=0 and 'Assertion' in run.stderr,(name,run.returncode,run.stderr[-1000:])
 results[name]={'exit':run.returncode,'assertion':run.stderr.splitlines()[-1]}
(out/'mutations.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results))
