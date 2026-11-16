from pathlib import Path
import subprocess,concurrent.futures,json
out=Path(__file__).resolve().parent
(out/'compile-only-public.key').write_bytes(bytes(range(32)))
def build(arch):
 with (out/f'build-{arch}.log').open('w') as log:
  subprocess.run(['python3','tools/pd-waveform/session/build.py','--arch',arch,'--output',str(out/arch)],stdout=log,stderr=subprocess.STDOUT,check=True)
 with (out/f'boot-{arch}.log').open('w') as log:
  r=subprocess.run(['python3','tools/pd-waveform/boot/build.py','--arch',arch,'--output',str(out/('boot-'+arch)),'--public-key',str(out/'compile-only-public.key')],stdout=log,stderr=subprocess.STDOUT)
 return arch,r.returncode
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool: results=dict(pool.map(build,['armv7','arm64']))
print(json.dumps(results));(out/'cross-results.json').write_text(json.dumps(results,indent=2)+'\n')
raise SystemExit(any(results.values()))
