import json,sys,time
from pathlib import Path
sys.path.insert(0,'/home/jeremy/AetherSDR/build/aetherpd-source/tools');from automation_probe import Bridge
out=Path(__file__).resolve().parent;label=sys.argv[1];assert label in ('off-01','on-01','off-02','on-02')
target=out/(label+'.json');assert not target.exists()
b=Bridge('/tmp/aetherpd-diagnostics')
def req(q):
 r=b.request(q,5);assert r.get('ok'),r;return r
req({'cmd':'invoke','target':'pdMeasureExport','action':'click'});time.sleep(.4)
req({'cmd':'invoke','target':'pdMeasureExportDialog/fileNameEdit','action':'setText','value':str(target)})
req({'cmd':'invoke','target':'pdMeasureExportDialog/&Save','action':'click'})
end=time.monotonic()+5
while not target.exists():assert time.monotonic()<end;time.sleep(.1)
x=json.loads(target.read_text());assert x['raw_iq_format']=='f32le-iq-v1' and x['source']=='local-dax-iq'
assert x['correction_active']==(label.startswith('on-'));print(json.dumps({'file':str(target),'bytes':target.stat().st_size,'correction_active':x['correction_active'],'valid':x['measurement']['valid']}));b.close()
