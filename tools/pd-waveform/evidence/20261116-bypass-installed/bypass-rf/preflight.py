"""Read-only live prerequisite check for the diagnostics RF comparison."""
import base64,hashlib,json,math,os,pathlib,struct,subprocess,sys
p=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(p.parents[1]/'aetherpd-source/tools'))
from automation_probe import Bridge
r={};instance=json.loads((p/'instance.json').read_text());pid=instance['pid'];proc=pathlib.Path('/proc')/str(pid)
assert proc.stat().st_uid==os.getuid()
assert hashlib.sha256((proc/'exe').read_bytes()).hexdigest()==instance['binary_sha256']
b=Bridge(instance['socket'])
def req(path):
 q=subprocess.run([str(p.parent/'first-iq-live/vault-v3'),path],capture_output=True,text=True,timeout=22)
 x=json.loads(q.stdout);assert q.returncode==0 and x['http_status']==200,x;return x['body']
for m in ('radio','transmit','slices','pans'):r[m]=b.request({'cmd':'get','model':m},5)[m]
assert r['radio']['connected'] and not r['radio']['transmitting'] and not r['radio']['fullDuplex']
assert r['radio']['serial']=='2125-1213-8600-8895'
assert r['transmit']['rfPower']==5 and r['transmit']['maxPowerLevel']<=10 and not r['transmit']['apdEnabled']
assert len(r['slices'])==1 and r['slices'][0]['frequency']==7.15 and r['slices'][0]['txAntenna']=='ANT1' and r['slices'][0]['rxAntenna']=='RX_A'
assert len(r['pans'])==1 and r['pans'][0]['ownedByUs'] and r['pans'][0]['rfGain']==0
r['installation']=req('aetherpd/installation')
assert r['installation']['installation_committed'] and r['installation']['selected_release_blake2b_256']==json.loads((p.parent/'upgrade-bypass-readiness/short/manifest.json').read_text())['release']['blake2b_256']
r['status']=req('aetherpd/status');assert r['status']['registered'] and not r['status']['keyed']
r['capture']=req('aetherpd/capture');c=r['capture'];assert not c['keyed'] and c['sample_count']==16384 and c['sample_rate_hz']==48000
binding=c['feedback_binding'];assert binding['valid'] and not binding['failed'] and binding['channel']==4 and binding['transport']=='local_dax_iq' and binding['gui_handle']==int(r['pans'][0]['clientHandle'],16)
raw=bytearray()
for i in range(32):
 part=req('aetherpd/capture/'+c['capture_id']+'/'+str(i));assert part['offset']==i*512
 chunk=base64.b64decode(part['iq_f32le'],validate=True);assert len(chunk)==4096;raw.extend(chunk)
values=struct.unpack('<32768f',raw);assert all(math.isfinite(v) for v in values) and any(v for v in values)
(p/'preflight-iq.f32le').write_bytes(raw);r.update(iq_sha256=hashlib.sha256(raw).hexdigest(),iq_bytes=len(raw),rms=math.sqrt(sum(v*v for v in values)/len(values)),passed=True)
(p/'preflight.json').write_text(json.dumps(r,indent=2))
config={'pid':pid,'start_ticks':int((proc/'stat').read_text().rsplit(')',1)[1].split()[19]),'binary_sha256':instance['binary_sha256'],'gui_handle':binding['gui_handle']}
(p/'observer-config.json').write_text(json.dumps(config,indent=2))

print(json.dumps({'passed':True,'iq_bytes':len(raw),'rms':r['rms'],'RF':False}))
