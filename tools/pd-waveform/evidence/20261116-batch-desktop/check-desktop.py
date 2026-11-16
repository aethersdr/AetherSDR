import shlex,subprocess,os,json
from pathlib import Path
out=Path(__file__).resolve().parent;project=out.parent/'first-install-desktop-monitor/project'
lines=subprocess.check_output(['ninja','-C',str(project),'-t','commands','aetherpd_capture_source_test'],text=True).splitlines()
objects={}
for name in ['AetherPDProtocol.cpp','AetherPDCaptureSource.cpp','aetherpd_capture_source_test.cpp']:
 args=shlex.split(next(l for l in lines if l.endswith('/'+name)))
 if args[0]=='ccache':args=args[1:]
 old=args[args.index('-o')+1];new=str(out/(name+'.asan.o'));objects[old]=new
 clean=[];i=0
 while i<len(args):
  if args[i] in ['-MF','-MT']:i+=2;continue
  if args[i]=='-MD':i+=1;continue
  clean.append(new if args[i]==old else args[i]);i+=1
 subprocess.run(clean+['-fsanitize=address,undefined','-fno-omit-frame-pointer'],cwd=project,check=True)
link=shlex.split(lines[-1]);assert link[:2]==[':','&&'] and link[-2:]==['&&',':']
link=[objects.get(a,a) for a in link[2:-2] if not a.startswith('-Wl,--dependency-file=')]
link[link.index('-o')+1]=str(out/'capture-asan')
subprocess.run(link+['-fsanitize=address,undefined'],cwd=project,check=True)
print('Built scoped desktop model sanitizer')
