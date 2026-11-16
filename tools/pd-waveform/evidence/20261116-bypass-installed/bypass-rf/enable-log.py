import json,pathlib,sys
p=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(p.parents[1]/'aetherpd-source/tools'));from automation_probe import Bridge
b=Bridge('/tmp/aetherpd-bypass')
r=b.request({'cmd':'log','action':'set','value':'aether.waveform.capture on'},5);assert r['ok'],r
r=b.request({'cmd':'log','action':'get','value':'aether.waveform.capture'},5);assert r['ok'],r
logs=p.parent/'guardian-owner-live/fresh/private-profile/logs'
f=max(logs.glob('aethersdr-*.log'),key=lambda x:x.stat().st_mtime)
meta={'path':str(f),'offset':f.stat().st_size,'scope':'Both off-01 and on-01; enabled explicitly after desktop restart','bridge':r}
(p/'log-offset.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta))
