import hashlib,json,os,pathlib,subprocess,sys,time
sys.path.insert(0,'/home/jeremy/AetherSDR/build/aetherpd-source/tools');from automation_probe import Bridge
p=pathlib.Path(__file__).resolve().parent;b=Bridge('/tmp/aetherpd-usb-start')
who=b.request({'cmd':'whoami'},5);assert who['pid']==2037172 and who['automationIdentity']=='aetherpd-shim-integration'
r=b.request({'cmd':'get','model':'radio'},5)['radio'];assert r['connected'] and not r['transmitting'] and not r['fullDuplex']
x=json.loads(subprocess.run([str(p.parent/'first-iq-live/vault-v3'),'aetherpd/installation'],capture_output=True,text=True,timeout=20).stdout);assert x['http_status']==200 and x['body']['installation_committed'] and x['body']['selected_release_blake2b_256']=='9de3a15952c806ea85a4248f7920d2ac2f0eff13b5c98cb4d4cd94b6acddf2ce'
assert hashlib.sha256(pathlib.Path('/proc',str(who['pid']),'exe').read_bytes()).hexdigest()=='571e811bfd871c83adabd47cda863809c44add9e0b82f0086c4813c4e3e7f847'
binary=p.parent/'first-install-desktop-monitor/project/AetherSDR';digest=hashlib.sha256(binary.read_bytes()).hexdigest();assert digest==json.loads((p/'desktop-provenance.json').read_text())['sha256']
assert b.request({'cmd':'invoke','target':'Quit','action':'trigger'},5)['ok'];b.close();end=time.monotonic()+15
while pathlib.Path('/proc',str(who['pid'])).exists():assert time.monotonic()<end;time.sleep(.2)
assert subprocess.run(['pgrep','-x','AetherSDR'],capture_output=True).returncode!=0
profile=p.parent/'guardian-owner-live/fresh/private-profile'
env=os.environ.copy();env.pop('AETHER_AUTOMATION_NO_TX',None);env.update(AETHER_SETTINGS_DIR=str(profile),AETHER_AUTOMATION='1',AETHER_AUTOMATION_ALLOW_TX='1',AETHER_AUTOMATION_IDENTITY='aetherpd-shim-integration',AETHER_AUTOMATION_AGENT_NAME='Codex AetherPD',AETHER_AUTOMATION_SOCKET='aetherpd-cleanup-retirement')
with (p/'desktop.log').open('ab') as log:proc=subprocess.Popen([str(binary)],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
r={'pid':proc.pid,'socket':'/tmp/aetherpd-cleanup-retirement','binary_sha256':digest,'profile':str(profile),'runtime_source_before_update':'489b1ed5fbbedefed448feb1078e2b1fbb5522d4','desktop_source':json.loads((p/'desktop-provenance.json').read_text())['source_commit'],'keying_requires_explicit_bridge_action':True};(p/'instance.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
