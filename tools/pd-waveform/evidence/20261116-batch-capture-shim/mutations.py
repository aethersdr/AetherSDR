import json, os, pathlib, subprocess
w=pathlib.Path('/home/jeremy/AetherSDR/build/aetherpd-source/tools/flex-tailnet-shim')
e=pathlib.Path(__file__).parent
cases=[
 ('frame_index','aetherpd_capture_batch_linux.go','binary.BigEndian.Uint32(data[96:100]) != offset/aetherPDCaptureSamples ||','false ||','^TestPDCaptureBatchParserBounds$'),
 ('correction_identity','aetherpd_capture_batch.go','a.Rate == b.Rate && a.Stream == b.Stream && a.Flags == b.Flags &&','a.Rate == b.Rate && a.Stream == b.Stream &&','^TestPDCaptureBatchRejectsMixedFrames/field68_at16384$'),
 ('owner_fence','aetherpd_capture_batch_handler.go','if !current || ctx.Err()','if (!current && false) || ctx.Err()','^TestPDCaptureBatchPublicOwnerFence/successor$'),
 ('cancel_retention','aetherpd_session_wire.go',' && !pdCaptureBatchOpcode(op)','', '^TestPDCaptureBatchSessionCancellation$')]
env=dict(os.environ,GOTOOLCHAIN='local',GOPROXY='off',GOCACHE=str(e.parent/'go-cache'))
go='/home/jeremy/go/pkg/mod/golang.org/toolchain@v0.0.1-go1.27.1.linux-amd64/bin/go'
results=[]
for name,file,old,new,test in cases:
 source=w/file
 text=source.read_text()
 assert old in text
 mutant=e/(name+'.go');mutant.write_text(text.replace(old,new))
 overlay=e/(name+'-overlay.json');overlay.write_text(json.dumps({'Replace':{str(source):str(mutant)}}))
 result=subprocess.run([go,'test','-mod=vendor','-overlay='+str(overlay),'-run',test,'-count=1','-timeout=15s','.'],cwd=w,env=env,capture_output=True,text=True)
 output=result.stdout+result.stderr
 (e/(name+'-mutation.log')).write_text(output)
 detected=result.returncode!=0 and '--- FAIL:' in output and '[build failed]' not in output
 results.append({'name':name,'returncode':result.returncode,'behavioral_failure_detected':detected})
 print(name,detected,flush=True)
 if not detected: raise RuntimeError(output)
(e/'mutation-results.json').write_text(json.dumps(results,indent=2)+'\n')
