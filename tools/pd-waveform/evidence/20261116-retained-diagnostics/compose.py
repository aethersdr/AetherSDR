import json, os, subprocess
from pathlib import Path
out=Path(__file__).resolve().parent
b=out.parent/'first-install-desktop-monitor/project'
w=out.parent.parent/'aetherpd-source'
go='/home/jeremy/go/pkg/mod/golang.org/toolchain@v0.0.1-go1.27.1.linux-amd64/bin/go'
fixtures=[]
for target in ['pd_calibration_dispatch_test','pd_dispatch_deadline_test']:
 r=subprocess.run([str(b/target)],capture_output=True,text=True)
 (out/(target+'-composition.log')).write_text(r.stdout+r.stderr);assert r.returncode==0
 fixtures.extend(json.loads(line.removeprefix('DIAGNOSTIC_FIXTURE ')) for line in r.stdout.splitlines() if line.startswith('DIAGNOSTIC_FIXTURE '))
assert len(fixtures)==4
results=[]
for i,fixture in enumerate(fixtures):
 native=out/f'native-fixture-{i}.json';public=out/f'public-fixture-{i}.json'
 native.write_text(json.dumps(fixture)+'\n')
 env=dict(os.environ,AETHER_PD_DIAGNOSTIC_NATIVE=str(native),AETHER_PD_DIAGNOSTIC_PUBLIC=str(public),GOTOOLCHAIN='local',GOPROXY='off',GOCACHE=str(out/'go-cache'))
 r=subprocess.run([go,'test','-mod=vendor','-race','-count=1','-run','^TestPDDiagnosticsNativeProjection$','.'],cwd=w/'tools/flex-tailnet-shim',env=env,capture_output=True,text=True)
 (out/f'composition-go-{i}.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr+r.stdout
 r=subprocess.run([str(b/'aetherpd_protocol_test')],env=env,capture_output=True,text=True)
 (out/f'composition-qt-{i}.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr+r.stdout
 assert json.loads(public.read_text())['diagnostics']==fixture['diagnostics']
 results.append({'case':i,'diagnostics':fixture['diagnostics'],'native_go_qt':True})
(out/'composition.json').write_text(json.dumps(results,indent=2)+'\n')
print('Four native/shim/desktop compositions passed')
