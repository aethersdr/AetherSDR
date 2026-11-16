import json,subprocess,sys,types
from pathlib import Path
out=Path(__file__).resolve().parent
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source')
sys.path.insert(0,str(root/'tools/pd-waveform/boot'))
import build
original=build.subprocess
def run(args,**kwargs):
 args=[a for a in args if a!=str(root/'tools/pd-waveform/boot/pd_upgrade_bootstrap.c')]
 args.extend(['-DPD_UPGRADE_TEST','-Wl,--wrap=fstat','-Wl,--wrap=fstatvfs'])
 (out/'budget-command.json').write_text(json.dumps(args,indent=2)+'\n')
 return subprocess.run(args,**kwargs)
try:
 build.subprocess=types.SimpleNamespace(run=run)
 print(build.build(out/'budget-build','host',str(out/'budget_probe.c')),flush=True)
finally:build.subprocess=original
