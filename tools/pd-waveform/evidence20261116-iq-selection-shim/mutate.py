from pathlib import Path
import os,subprocess,json,hashlib
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source');out=Path(__file__).resolve().parent
p=root/'tools/flex-tailnet-shim/aetherpd_binding.go';original=p.read_text();old='binding.Pan != id.FeedbackPan || ';assert original.count(old)==1
command=['go','test','-mod=vendor','-race','-count=1','-run','^TestPDSelectedStartRequiresObservedBinding$', '.']
env=dict(os.environ,GOCACHE=str(out.parent/'go-cache'))
result={'mutation':'omit-selected-pan-confirmation'}
try:
 p.write_text(original.replace(old,''))
 with (out/'selected-pan-mutation.log').open('w') as log:run=subprocess.run(command,cwd=p.parent,env=env,stdout=log,stderr=subprocess.STDOUT)
 result.update(exit=run.returncode,expected_case_failed='wrong_pan' in (out/'selected-pan-mutation.log').read_text())
 assert run.returncode and result['expected_case_failed']
finally:
 p.write_text(original);result['restored_sha256']=hashlib.sha256(p.read_bytes()).hexdigest()
 with (out/'selected-pan-restored.log').open('w') as log:run=subprocess.run(command,cwd=p.parent,env=env,stdout=log,stderr=subprocess.STDOUT)
 result['restored_exit']=run.returncode;(out/'mutation.json').write_text(json.dumps(result,indent=2)+'\n');assert not run.returncode
print(result)
