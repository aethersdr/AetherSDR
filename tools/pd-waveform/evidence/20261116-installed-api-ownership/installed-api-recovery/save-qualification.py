from pathlib import Path
import datetime,gzip,hashlib,json,shutil
B=Path('/home/jeremy/AetherSDR/build/aetherpd-shim-integration');W=B.parent/'aetherpd-source';E=W/'tools/pd-waveform/evidence/20261116-installed-api-ownership'
E.mkdir(parents=True,exist_ok=True)
a=json.loads((B/'installed-api-ownership/outcome.json').read_text());f=json.loads((B/'installed-unkey-api-loss/outcome.json').read_text());r=json.loads((B/'installed-api-recovery/usb-receive-iq-check.json').read_text());outer=json.loads((B/'installed-api-recovery/final-audit-result.json').read_text());audit=json.loads(outer['logs'])
assert a['passed'] and f['passed'] and r['passed'] and audit['passed']
for name in ['installed-unkey-api-loss/disconnect-result.json','installed-api-recovery/final-audit-result.json']:
 x=json.loads((B/name).read_text());assert x['wait']['StatusCode']==0 and x['job_container_deleted'] and x['temporary_host_directory_deleted']
for folder,names in {
 'installed-api-ownership':['run.py','vault-ownership.cpp','build-command.json','build.log','events.json','outcome.json'],
 'installed-unkey-api-loss':['run.py','run-job.py','fault-template.py','disconnect-job.py','disconnect-result.json','events.json','outcome.json'],
 'installed-api-recovery':['receive-iq-check.py','instance.json','usb-receive-iq-check.json','final-audit-job.py','final-audit-result.json','run-job.py','save-qualification.py'],
}.items():
 o=E/folder;o.mkdir(exist_ok=True)
 for name in names:shutil.copy2(B/folder/name,o/name)
raw=(B/'installed-api-recovery/usb-receive-iq.f32le').read_bytes();assert len(raw)==r['iq_bytes']==131072 and hashlib.sha256(raw).hexdigest()==r['iq_sha256']
(E/'installed-api-recovery/usb-receive-iq.f32le.gz').write_bytes(gzip.compress(raw,mtime=0))
p=f['probe'];assert p['probe_complete'] and not p['signals'] and not p['rf_commands'] and len(p['commands'])==1
assert len(p['original_worker_pid_checks_including_zombies'])==9 and len(audit['processes'])==3
assert len(a['auth_rejections'])==10 and all(x['http_status']==401 for x in a['auth_rejections'])
assert a['duplicate_start']['http_status']==409 and all(x['http_status']==409 for x in a['concurrent_duplicate_starts']) and a['unknown_executable_field']['http_status']==400 and a['foreign_gui_start']['http_status']==503
now=datetime.datetime.now(datetime.timezone.utc).isoformat()
report={
 'updated_utc':now,'status':'installed_receive_only_auth_ownership_and_unkey_API_loss_qualified','production_ready':False,
 'native_source_commit':'a87c915908e73588142e1148303d2c3c83d57a45',
 'installed_release_blake2b_256':'bf6df6a8a9f1d70ca26218e105bb949166b22d3ed911686e72877789ab040dab',
 'installed_upgrade_plan_blake2b_256':'56a1b78e03441fd6ff9eac942e7b8aa9831c46381a4511666cfefebf33602956',
 'desktop':json.loads((B/'cleanup-retirement-live/instance.json').read_text()),
 'radio':{'model':'FLEX-8600','serial':'2125-1213-8600-8895','firmware':a['radio_after']['version'],'boot_id':audit['boot_id'],'FDX':False,'RF_commands':[]},
 'installed_authorization_boundary':{
  'missing_and_invalid_credentials':{'requests':a['auth_rejections'],'all_401':True,'session_stayed_registered_and_IQ_binding_valid':True},
  'duplicate_start':{'sequential_HTTP':a['duplicate_start']['http_status'],'concurrent_HTTP':[x['http_status'] for x in a['concurrent_duplicate_starts']],'scope':'Two concurrent requests while an existing session already owned the slot; not a race between two independent GUIs from an empty slot.'},
  'untrusted_extra_executable_field':a['unknown_executable_field'],
  'foreign_nonexistent_GUI_identity':{'HTTP':a['foreign_gui_start']['http_status'],'never_published_available_or_registered':True,'post_refusal_status':a['after_foreign'],'scope':'Fixed nonexistent UUID paired with the actual current GUI handle and IQ selection; not an actual second GUI.'},
  'live_IQ_before_and_after':{'before':a['capture_before'],'after':a['capture_after'],'same_source_generation':a['capture_before']['source']['generation']==a['capture_after']['source']['generation']},
  'credential_mutations':[],'credential_exposed_in_output':False,
  'probe_helper_sha256':hashlib.sha256((B/'installed-api-ownership/vault-ownership').read_bytes()).hexdigest(),
 },
 'owned_unkey_API_disconnect':{
  'command_authority':'Local open-source reference/FlexLib/FlexLib/Radio.cs DisconnectClientByHandle, line2625; client disconnect plus exact handle.',
  'command':p['commands'][0],'ack':p['disconnect_ack'],'ack_ms':p['ack_ms'],'guard_exit_ms':p['guard_exit_ms'],'all_original_workers_gone_ms':p['workers_gone_ms'],
  'target':p['target'],'target_socket':p['target_api_socket'],'ownership_admission':'Exact SHA256, UID78666, zero capability sets, NNP1, current GUI handle, only lifetime guardian in the DSP session cgroup, active loopback4992 socket and fresh idle interlock.',
  'original_worker_count':9,'original_PIDs_checked_including_zombies':p['original_worker_pid_checks_including_zombies'],
  'automatic_worker_restart_observed':False,'observation_after_workers_gone_s':1,'public_status_after':f['status_after'],
  'ARP_after':p['arp_after'],'journal_after':p['journal_after'],'accept_local':p['accept_local'],'signals_sent':p['signals'],
  'probe_container_deleted':True,'temporary_host_directory_deleted':True,
 },
 'ordinary_explicit_recovery':{
  'passed':True,'IQ_transport':'local_dax_iq','FDX':False,'sample_rate_hz':48000,'complex_samples':16384,'IQ_bytes':r['iq_bytes'],'IQ_sha256':r['iq_sha256'],'nonzero_scalar_values':r['nonzero_values'],'finite_scalar_values_checked':True,
  'start_was_explicit_GUI_action':True,'privileged_repair':False,'final_status':r['stopped'],
 },
 'final_cleanup':{'processes':audit['processes'],'ARP':audit['arp'],'accept_local':audit['accept_local'],'identity_sha256':audit['identity_sha256'],'runtime_hashes':audit['runtime_hashes'],'temporary_installer_paths':audit['temporary_paths'],'owned_upgrade_carrier_artifacts':audit['owned_upgrade_carrier_artifacts'],'probe_container_deleted':outer['job_container_deleted'],'temporary_host_directory_deleted':outer['temporary_host_directory_deleted']},
 'limitations':[
  'Receive-only: no RF, calibrated correction, keyed unkey timing, native9second deadline, sustained RF or stockAPD comparison in these probes.',
  'Invalid/missing credential rejection is qualified; successful credential rotation/signout during an installed session is not qualified. The current API replaces or removes node identity during these operations; no disposable subordinate credential exists.',
  'Only the owned unkey guardian API connection was deliberately disconnected. Other API roles and simultaneous owner failures remain separate gates.',
  'Ownership rejection covers existing-session duplicates and a nonexistent GUI, not two real competing GUI clients from an empty slot.',
  'Public cleanup_unconfirmed is retained after abnormal API exit even though the independent process/ARP audit verifies retirement; no success is inferred from that public state.',
  'Three unprivileged control processes remain at the stopped baseline; no claim that the control owner or entire session cgroup was immediately removed.',
  'Temporary broad diagnostics bridge used only to pin and fault the exact installed API client and audit state, not for session startup, IQ recovery, normal installation or shipping delivery.',
  'Actual FLEX6300 Linux2.6/CortexA8 qualification and independent release gates remain open; ARM64 on8600 does not prove them.',
 ],
}
report['evidence_sha256']={str(p.relative_to(W)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(E.rglob('*')) if p.is_file()}
path=W/'tools/pd-waveform/INSTALLED-API-OWNERSHIP-QUALIFICATION.json';tmp=path.with_suffix('.json.tmp');tmp.write_text(json.dumps(report,indent=2)+'\n');tmp.replace(path)
planpath=B/'production-completion-plan-20261113.json';plan=json.loads(planpath.read_text());plan['updated_utc']=now
plan['installed_API_ownership_qualification']={'status':report['status'],'report':str(path),'source_commit':report['native_source_commit'],'unkey_API_disconnect_worker_cleanup_ms':p['workers_gone_ms'],'invalid_credential_requests_rejected':10,'ordinary_receive_recovery_full_frame':True,'successful_credential_revocation_qualified':False,'RF_commands':[],'production_ready':False}
plan['current_candidate']['runtime_state']='stopped';plan['current_candidate']['RF_idle']=True
plan['current_focus']='Remaining installed partial-stage, credential/reconnect and independent-owner failure qualification; receive-only while independent RF reference policy remains unresolved.'
plan['next_action']='Continue unqualified installed partial capture/DSP stage and API-role/credential-revocation tests. Do not repeat completed baseline source-stage, root-retirement, idle-stall, duplicate-owner, unkey-API-loss or ordinary reboot/IQ tests. Keep RF idle while LP100A remains deferred; preserve P1–P10 including actual6300 and final RF/stockAPD/release gates.'
tmp=planpath.with_suffix('.json.tmp');tmp.write_text(json.dumps(plan,indent=2)+'\n');tmp.replace(planpath)
print(json.dumps({'report':str(path),'evidence_files':len(report['evidence_sha256']),'production_ready':False}))
