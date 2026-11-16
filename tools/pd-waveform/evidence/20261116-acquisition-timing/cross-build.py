import importlib.util, json, hashlib
from pathlib import Path
w=Path('/home/jeremy/AetherSDR/build/aetherpd-source')
out=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('dsp_build', w/'tools/pd-waveform/build.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
results={}
for arch,target in [('armv7',m.ARMV7),('arm64',m.ARM64)]:
    binary=out/('aether-pd-'+arch)
    m.zig_cc(target,binary,out/('zig-cache-'+arch))
    results[arch]={'bytes':binary.stat().st_size,'sha256':hashlib.sha256(binary.read_bytes()).hexdigest()}
(out/'cross-builds.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results))
