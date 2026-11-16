import hashlib,json,os,pathlib,subprocess,sys,time
sys.path.insert(0,'/home/jeremy/AetherSDR/build/aetherpd-source/tools');from automation_probe import Bridge
p=pathlib.Path(__file__).resolve().parent;b=Bridge('/tmp/aetherpd-desktop-stall')
who=b.request({'cmd':'whoami'},5);assert who['pid']==1337646 and who['automationIdentity']=='aetherpd-shim-integration'
r=b.request({'cmd':'get','model':'radio'},5)['radio'];assert r['connected'] and not r['transmitting'] and not r['fullDuplex']
x=json.loads(subprocess.run([str(p.parent/'first-iq-live/vault-v3'),'aetherpd/status'],capture_output=True,text=True,timeout=20).stdout);assert x['body']['state']=='stopped'
binary=p.parent/'first-install-desktop-monitor/project/AetherSDR';digest=hashlib.sha256(binary.read_bytes()).hexdigest();assert digest=='6fa6dd740a75b45408ece21917355aedfcd782253e7c6f9dad4d64733a5a84d6'
assert b.request({'cmd':'invoke','target':'Quit','action':'trigger'},5)['ok'];b.close();end=time.monotonic()+15
while pathlib.Path('/proc',str(who['pid'])).exists():assert time.monotonic()<end;time.sleep(.2)
assert subprocess.run(['pgrep','-x','AetherSDR'],capture_output=True).returncode!=0
profile=p.parent/'guardian-owner-live/fresh/private-profile'
env=os.environ.copy();env.pop('AETHER_AUTOMATION_NO_TX',None);env.update(AETHER_SETTINGS_DIR=str(profile),AETHER_AUTOMATION='1',AETHER_AUTOMATION_ALLOW_TX='1',AETHER_AUTOMATION_IDENTITY='aetherpd-shim-integration',AETHER_AUTOMATION_AGENT_NAME='Codex AetherPD',AETHER_AUTOMATION_SOCKET='aetherpd-release-recovery')
with (p/'desktop.log').open('ab') as log:proc=subprocess.Popen([str(binary)],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
r={'pid':proc.pid,'socket':'/tmp/aetherpd-release-recovery','binary_sha256':digest,'profile':str(profile),'runtime_source':'e8d68ad4bcfca1bf87f1d95b6c613cda17fac097','keying_requires_explicit_bridge_action':True};(p/'instance.json').write_text(json.dumps(r,indent=2));print(json.dumps(r))
