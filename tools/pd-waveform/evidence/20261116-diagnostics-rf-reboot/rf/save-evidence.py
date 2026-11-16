"""Archive exact-build RF successes, the failed first attempt, and reboot proof."""
import datetime, hashlib, json, pathlib, re, shutil
p=pathlib.Path(__file__).resolve().parent
w=p.parents[1]/'aetherpd-source';pd=w/'tools/pd-waveform'
def read(f):return json.loads(f.read_text())
def write(f,x):
 t=f.with_suffix(f.suffix+'.new');t.write_text(json.dumps(x,indent=2)+'\n');t.replace(f)
r=read(p.parent/'diagnostics-reboot/reboot-check.json')
iq=read(p.parent/'diagnostics-reboot/receive-iq-check.json')
assert r['passed'] and r['reboot_commands_sent']==1 and r['boot_id_before']!=r['boot_id_after'] and iq['passed']
assert r['before']==r['after'] and r['identity_before']['installation_id']==r['identity_after']['installation_id']
audit=read(p.parent/'guardian-owner-live/diagnostics-reboot-idle-result.json');host=json.loads(audit['logs'])
assert host['passed'] and audit['job_container_deleted'] and audit['temporary_host_directory_deleted']
cases=read(p/'cases.json');comparison=read(p/'comparison.json')
assert not cases['off-01']['valid_measurement'] and cases['off-02']['valid_measurement'] and cases['on-02']['valid_measurement']
assert all(c['released'] and not any(c['faults_at_end'].values()) and c['input_limited_samples']==0 for c in cases.values())
dest=pd/'evidence/20261116-diagnostics-rf-reboot';dest.mkdir(exist_ok=False)
for root,prefix in [(p,'rf'),(p.parent/'diagnostics-reboot','reboot')]:
 for source in sorted(root.rglob('*')):
  if not source.is_file() or '__pycache__' in source.parts:continue
  target=dest/prefix/source.relative_to(root);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
for label in ['diagnostics-rf-idle','diagnostics-reboot-idle']:
 for suffix in ['-job.py','-result.json']:
  shutil.copyfile(p.parent/'guardian-owner-live'/(label+suffix),dest/(label+suffix))
manifest={}
for f in sorted(dest.rglob('*')):
 if not f.is_file():continue
 if f.suffix in ['.json','.py','.log']:
  text=f.read_text();assert not re.search(r'-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----|Bearer [A-Za-z0-9._~-]{16,}|"auth_token"\s*:\s*"[^\"]+"',text),str(f)
 manifest[str(f.relative_to(dest))]=hashlib.sha256(f.read_bytes()).hexdigest()
now=datetime.datetime.now(datetime.timezone.utc).isoformat()
report={'schema':1,'updated_utc':now,'production_ready':False,'runtime_source':'fa8dd7aecfe8e9e35a9e108bbd5f03481951093f','desktop_source':'fa8dd7aecfe8e9e35a9e108bbd5f03481951093f',
 'status':'One valid 5W OFF/ON pair, failed first cold capture retained, and one ordinary reboot followed by local IQ passed',
 'radio':{'model':'FLEX-8600','firmware':'4.3.16.42034','frequency_mhz':7.15,'nominal_watts':5,'configured_max_watts':10,'FDX':False,'feedback':'RX_A','transmit_antenna':'ANT1','preamp_db':0,'transport':'local_dax_iq','channel':4},
 'bounds':{'source_seconds':8,'independent_unkey_seconds':9,'desktop_authorization_seconds':10,'measured_abort_watts':10,'minimum_cooldown_seconds':31,'positive_key_actions':3,'automatic_retries':False},
 'cases':cases,'comparison':comparison,
 'reboot':{'commands':1,'ready_seconds':r['ready_s'],'boot_id_before':r['boot_id_before'],'boot_id_after':r['boot_id_after'],'same_installed_release':r['before']==r['after'],'identity_preserved':True,'receive_iq_bytes':iq['iq_bytes'],'receive_iq_sha256':iq['iq_sha256'],'receive_iq_rms_raw':iq['rms'],'session_stopped':iq['stopped']['body']['state']=='stopped'},
 'cleanup':{'pre_and_post_reboot_audits_passed':True,'temporary_audit_components_removed':True,'identity_sha256':host['identity_sha256'],'arp_empty':not host['arp'],'accept_local':host['accept_local'],'remaining_processes':host['processes']},
 'findings':['The first capture failed after normal native source_samples_complete; retained diagnostics show no alignment reset, stale IQ, rejected TX input, send error or dispatch deadline fault. It is not the historical unexplained cancellation.',
 'First observed ready tones arrived 6.905s after GUI intent in off-01 versus5.154s in off-02. These are host polling observations, not exact RF or native alignment timestamps. Delayed acquisition/readiness and capture latency need separation; timing correlation is not root-cause proof.',
 'Successful captures retain interlock_ineligible at orderly GUI release. This reason alone is not evidence of a startup fault; source ID, event time and current state must be considered.'],
 'limitations':['Three finite bench transmissions, not sustained speech or broad repeatability; first attempt failed.','The first post-reboot receive probe stopped before starting because the old dialog was invalidated. Closing/reopening the panel established the new radio session; this was not a runtime startup failure.','No new10W or stockAPD test on fa8; previous candidate results retain original attribution.','No calibrated absolute power or ADC headroom. LP100A deferred.','One ordinary reboot, not physical power loss or interrupted upgrade/rollback.','Cold-start cancellation and natural128sample slip remain unresolved; diagnostics show no alignment reset in these captures.','No actual6300, final process fault matrix, cross-platform or release qualification.'],
 'evidence_directory':str(dest.relative_to(pd)),'evidence_sha256':manifest}
write(pd/'DIAGNOSTICS-RF-REBOOT-QUALIFICATION.json',report)
progress=read(pd/'PROGRESS.json');progress['updated_utc']=now
progress['next_action']='Attribute first-key delayed tone readiness versus capture latency using retained evidence and targeted startup timing; complete cold/warm/speech recovery, exact-build rollback/fault matrix and actual6300 handoff. Do not discard failed off-01.'
progress['diagnostics_rf_reboot']={'report':'DIAGNOSTICS-RF-REBOOT-QUALIFICATION.json','finite_5w_pair_passed':True,'first_cold_capture_failed':True,'one_ordinary_reboot_passed':True,'historical_failures_resolved':False}
current=progress['current_delivery_status'];current['latest_live_report']='DIAGNOSTICS-RF-REBOOT-QUALIFICATION.json';current['installed_state']='fa8 desktop/runtime: finite5W pair, one ordinary reboot and post-reboot local IQ passed; first cold capture failed. Session stopped and cleanup audited.'
current['source_provenance_note']='New5W and one-reboot report attributes runtime/desktopfa8dd7aec. Previous5/10W and stock comparisons remain e8d68ad4b/b1256a165 evidence.'
current['latest_read_only_network_probe']={'utc':now,'address':'192.168.50.100','radio_mutations':False,'status':'Post-reboot receive session stopped; identity, process privileges and cleanup audited'}
write(pd/'PROGRESS.json',progress)
print(json.dumps({'evidence_files':len(manifest),'report':str(pd/'DIAGNOSTICS-RF-REBOOT-QUALIFICATION.json')}))
