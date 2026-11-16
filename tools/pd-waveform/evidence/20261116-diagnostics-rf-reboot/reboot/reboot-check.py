import json,pathlib,socket,subprocess,sys,time
sys.path.insert(0,'/home/jeremy/AetherSDR/build/aetherpd-source/tools');from automation_probe import Bridge
out=pathlib.Path(__file__).resolve().parent;report={'passed':False,'rf_commands':[],'reboot_commands_sent':0};start=time.monotonic()
ssh=['ssh','-F','/dev/null','-o','BatchMode=yes','-o','ConnectTimeout=3','-o','IdentitiesOnly=yes','-i','/home/jeremy/agent-worktrees/c3957d44/flexipv6-build/keys/probe','-o','UserKnownHostsFile=/tmp/aetherpd-shim-integration-kh-probe','-o','StrictHostKeyChecking=yes','-p','22222','root@192.168.50.100','cat /proc/sys/kernel/random/boot_id']
def status(path):
 p=subprocess.run([str(out.parent/'first-iq-live/vault-v3'),path],capture_output=True,text=True,timeout=22)
 assert p.returncode==0,'status unavailable'
 return json.loads(p.stdout)['body']
def boot():
 p=subprocess.run(ssh,capture_output=True,text=True,timeout=5);assert p.returncode==0
 s=p.stdout.strip();assert len(s)==36;return s
def listening(port):
 try:
  with socket.create_connection(('192.168.50.100',port),.4):return True
 except OSError:return False
try:
 assert not (out/'reboot-check.json').exists() and not (out/'reboot-intent.json').exists(), 'One reboot only; inspect prior attempt'
 audit=json.loads(json.loads((out.parent/'guardian-owner-live/diagnostics-rf-idle-result.json').read_text())['logs']);assert audit['passed']
 b=Bridge('/tmp/aetherpd-diagnostics');radio=b.request({'cmd':'get','model':'radio'},5)['radio'];assert radio['connected'] and not radio['transmitting'] and not radio['fullDuplex'];report['radio_before']=radio
 report['before']=status('aetherpd/installation');assert report['before']['installation_committed'] and report['before']['selected_release_blake2b_256']=='d4cb44485dade35a82e1d4c4ef6845ae3cce28dfe2d4ffefbc9c8497c9bb47ef'
 report['identity_before']=status('status');assert report['identity_before']['sessions']==0
 report['boot_id_before']=boot()
 assert status('aetherpd/status')['state']=='stopped'
 (out/'reboot-intent.json').write_text(json.dumps(report,indent=2))
 with socket.create_connection(('192.168.50.100',4992),3) as api:
  api.settimeout(2);api.recv(16384);api.sendall(b'C1|radio reboot\n');report['reboot_commands_sent']=1
  try:api.recv(16384)
  except OSError:pass
 deadline=time.monotonic()+45
 while time.monotonic()<deadline:
  if not listening(4992) and not listening(48992):report['both_down_s']=round(time.monotonic()-start,3);print('radio_down',flush=True);break
  time.sleep(.5)
 assert 'both_down_s' in report
 deadline=time.monotonic()+180
 while time.monotonic()<deadline:
  if listening(48992):
   try:
    after=status('aetherpd/installation')
    if after==report['before']:report['after']=after;report['ready_s']=round(time.monotonic()-start,3);print('exact_commit_ready',report['ready_s'],flush=True);break
   except (OSError,AssertionError,ValueError,subprocess.TimeoutExpired):pass
  time.sleep(.5)
 assert 'after' in report
 report['identity_after']=status('status');assert report['identity_after']['installation_id']==report['identity_before']['installation_id'] and report['identity_after']['state']=='local'
 # Diagnostics may come up after the primary service; wait independently.
 deadline=time.monotonic()+90
 while time.monotonic()<deadline:
  try:
   afterboot=boot()
   if afterboot!=report['boot_id_before']:report['boot_id_after']=afterboot;break
  except (OSError,AssertionError,subprocess.TimeoutExpired):pass
  time.sleep(.5)
 assert 'boot_id_after' in report
 report['radio_after']=b.request({'cmd':'get','model':'radio'},5)['radio'];assert not report['radio_after']['transmitting'];report['passed']=True
except Exception as e:report['error']=type(e).__name__+': '+str(e)
finally:
 report['elapsed_s']=round(time.monotonic()-start,3);(out/'reboot-check.json').write_text(json.dumps(report,indent=2));print(json.dumps({k:report[k] for k in ['passed','error','ready_s','boot_id_before','boot_id_after'] if k in report}),flush=True)
 if not report['passed']:raise SystemExit(1)
