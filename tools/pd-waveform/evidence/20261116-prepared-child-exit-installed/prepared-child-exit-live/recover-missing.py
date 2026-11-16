"""Launch once only after authoritative absence; never stop a foreign instance."""
import datetime,hashlib,json,os,pathlib,subprocess,time
P=pathlib.Path;out=P(__file__).resolve().parent;base=out.parent
assert not (out/'instance.json').exists(),'Do not replace a recorded instance'
assert not P('/proc/2177039').exists()
assert subprocess.run(['pgrep','-x','AetherSDR'],capture_output=True).returncode==1
assert not P('/tmp/aetherpd-prepared-child-exit').exists()
status=json.loads(subprocess.run([str(base/'first-iq-live/vault-v3'),'aetherpd/installation'],capture_output=True,text=True,timeout=22,check=True).stdout)
assert status['body']['selected_release_blake2b_256']=='bf6df6a8a9f1d70ca26218e105bb949166b22d3ed911686e72877789ab040dab'
assert json.loads((base/'upgrade-prepared-child-exit/report.json').read_text())['complete_eight_file_distribution_identical']
provenance=json.loads((out/'desktop-provenance.json').read_text());binary=P(provenance['binary']);assert hashlib.sha256(binary.read_bytes()).hexdigest()==provenance['sha256']
record={'checked_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'old_pid':2177039,'old_PID_absent':True,'other_AetherSDR_processes':[],'coredumpctl':'No coredumps found for PID2177039','kernel_OOM_entries_in_previous25minutes':0,'exit_cause':'unattributed; no claim that absence proves a crash','radio_before_launch':status,'RF_commands':[]}
(out/'missing-instance-observation.json').write_text(json.dumps(record,indent=2)+'\n')
profile=base/'guardian-owner-live/fresh/private-profile';env=os.environ.copy();env.pop('AETHER_AUTOMATION_NO_TX',None);env.update(AETHER_SETTINGS_DIR=str(profile),AETHER_AUTOMATION='1',AETHER_AUTOMATION_ALLOW_TX='1',AETHER_AUTOMATION_IDENTITY='aetherpd-shim-integration',AETHER_AUTOMATION_AGENT_NAME='Codex AetherPD',AETHER_AUTOMATION_SOCKET='aetherpd-prepared-child-exit')
with (out/'desktop.log').open('ab') as log:proc=subprocess.Popen([str(binary)],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
r={'pid':proc.pid,'socket':'/tmp/aetherpd-prepared-child-exit','binary_sha256':provenance['sha256'],'profile':str(profile),'runtime_source_before_update':'a87c915908e73588142e1148303d2c3c83d57a45','desktop_source':provenance['source_commit'],'keying_requires_explicit_bridge_action':True};(out/'instance.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r),flush=True)
