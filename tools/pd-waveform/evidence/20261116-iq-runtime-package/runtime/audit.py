from pathlib import Path
import sys,subprocess,json,hashlib,stat
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source');out=Path(__file__).resolve().parent
sys.path.insert(0,str(root/'tools/pd-waveform/boot'))
import build,bundle,release
checker=build.build(out/'audit-build','host','bundle_check.c')
rows=[]
def run(args,expected):
 r=subprocess.run(list(map(str,args)),capture_output=True,timeout=20)
 assert r.returncode==expected,(args,r.returncode,r.stderr.decode())
for arch,number,profile,firmware in [('armv7',7,2,'4.2.18.41174'),('arm64',64,1,'4.3.16.42034')]:
 d=out/'short'/arch;meta=json.loads((d/'manifest.json').read_text());work=out/'audit'/arch;work.mkdir(parents=True,exist_ok=False)
 artifact=d/'candidate.release';key=d/'public-key';digest=d/'release-digest';native=d/'verify/release-check'
 # Use the native checker path emitted by the builder, without assuming spelling.
 native=next((d/'verify').glob('release*check'))
 extracted=work/'release';extracted.mkdir()
 command=[native,artifact,key,number,profile,firmware,digest]
 run(command+[extracted],0)
 for entry in meta['release']['files']:
  p=extracted/entry['name'];assert p.stat().st_size==entry['bytes'];assert hashlib.sha256(p.read_bytes()).hexdigest()==entry['sha256']
 raw=work/'payload';raw.mkdir();run([checker,extracted/'payload.bundle',key,number,raw],0)
 payload=[]
 for entry in meta['payload']['files']:
  p=raw/entry['name'];assert p.stat().st_size==entry['bytes'];assert hashlib.sha256(p.read_bytes()).hexdigest()==entry['sha256']
  expected=0o644 if entry['name']=='LICENSES' else 0o755
  assert stat.S_IMODE(p.stat().st_mode)==expected
  payload.append({'name':p.name,'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'mode':oct(expected)})
 run([checker,extracted/'payload.bundle',key,number,raw],1)
 failures=[];good=artifact.read_bytes()
 for label,changed in [('header',0),('manifest',48),('signature',release.MANIFEST),('payload',len(good)-1)]:
  data=bytearray(good);data[changed]^=1
  candidate=work/(label+'.release');candidate.write_bytes(data);approved=work/(label+'.digest');approved.write_bytes(bundle.blake(data))
  run([native,candidate,key,number,profile,firmware,approved],1);failures.append(label)
 for label,data in [('truncated',good[:-1]),('trailing',good+b'x')]:
  candidate=work/(label+'.release');candidate.write_bytes(data);approved=work/(label+'.digest');approved.write_bytes(bundle.blake(data))
  run([native,candidate,key,number,profile,firmware,approved],1);failures.append(label)
 wrongkey=work/'wrong-public';wrongkey.write_bytes(bytes(32));run([native,artifact,wrongkey,number,profile,firmware,digest],1);failures.append('wrong public key')
 for label,n,p,f in [('wrong architecture',64 if number==7 else 7,profile,firmware),('wrong profile',number,1 if profile==2 else 2,firmware),('wrong firmware',number,profile,'4.0.0.0')]:
  run([native,artifact,key,n,p,f,digest],1);failures.append(label)
 rows.append({'arch':arch,'release_files_exact':len(meta['release']['files']),'payload_files':payload,'native_extraction_matches_manifest':True,'existing_payload_not_overwritten':True,'rejected_with_recomputed_approval_digest':failures})
(out/'audit-report.json').write_text(json.dumps(rows,indent=2)+'\n');print('Both actual release archives authenticated, extracted, hash/mode checked and tamper-tested')
