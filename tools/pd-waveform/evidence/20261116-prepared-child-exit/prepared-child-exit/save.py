from pathlib import Path
import datetime,hashlib,json,shutil
B=Path('/home/jeremy/AetherSDR/build/aetherpd-shim-integration');W=B.parent/'aetherpd-source';E=W/'tools/pd-waveform/evidence/20261116-prepared-child-exit';E.mkdir(parents=True,exist_ok=True)
old=json.loads((B/'installed-capture-startup/outcome.json').read_text());assert old['passed'] and not old['registered_was_observed'];p=old['probe'];assert p['probe_complete']
verify=json.loads((B/'prepared-child-exit/verification.json').read_text());assert verify['host_policy_passed'] and verify['ASan_UBSan_LSan_passed'] and len(verify['mutations'])==3
for folder,names in {
 'installed-capture-startup':['run-once.py','run-job.py','fault-template.py','capture-job.py','capture-result.json','events.json','outcome.json'],
 'prepared-child-exit':['verify.py','verification.json','save.py','host-test.log','sanitizer-run.log','ctest-build.log','ctest-run.log','build-armv7.log','build-arm64.log','mutation-no-exit-observation.log','mutation-reap-during-observation.log','mutation-signal-before-ownership.log'],
}.items():
 o=E/folder;o.mkdir(exist_ok=True)
 for name in names:shutil.copy2(B/folder/name,o/name)
report={
 'updated_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'status':'component_fix_verified_installed_requalification_pending','production_ready':False,'installed_fix':False,
 'baseline':{'native_source':'a87c915908e73588142e1148303d2c3c83d57a45','release_blake2b_256':'bf6df6a8a9f1d70ca26218e105bb949166b22d3ed911686e72877789ab040dab','boot_id':p['boot_id'],'desktop_PID':2177039},
 'installed_capture_startup_failure':{'passed':old['passed'],'target':p['target'],'signals':p['signals'],'never_observed_registered':not old['registered_was_observed'],'workers_gone_ms':p['workers_gone_ms'],'observations':p['observations'],'all_original_worker_PIDs_checked_including_zombies':p['original_worker_pid_checks_including_zombies'],'ARP_after':p['arp_after'],'journal_after':p['journal_after'],'public_status_after':old['status_after'],'RF_commands':[]},
 'root_cause':'The inherited readiness socket is a Unix datagram endpoint. DSP exit need not produce a readiness datagram or poll hangup, so prepared_wait_ready could keep its root caller in the10second poll loop after DSP exit.',
 'change':'Inspect only the forked child with waitid(P_PID, WEXITED|WNOHANG|WNOWAIT) in readiness/profile waits, refusing on exit or ownership error without reaping. Before cleanup signals, waitpid(WNOHANG) confirms a live waitable child; exited/lost children receive no signal.',
 'source_sha256':{name:hashlib.sha256((W/name).read_bytes()).hexdigest() for name in ['tools/pd-waveform/session/pd_prepared_launch.c','tools/pd-waveform/session/radio_launch_test.c','tests/tests.cmake']},
 'verification':verify,'registered_CTest':{'name':'pd_radio_launch_policy_test','built_modified_source':True,'passed':True,'scope':'Own child processes and inherited Unix datagram gate; no firmware peer, RF or privileges.'},
 'build_receipt_note':'An initial CMake build failed because the default ccache path was read-only; the following old-binary CTest is not counted. Saved ctest-build/run logs prove the subsequent successful modified-source build using the dedicated writable cache.',
 'kernel_compatibility':{'source':'https://man7.org/linux/man-pages/man2/waitpid.2.html','documented_waitid_since':'Linux2.6.9','WNOWAIT':'Leaves the child waitable; no pidfd requirement in shipping code.','actual_FLEX6300_qualified':False},
 'limitations':['The installed baseline startup-failure refusal and cleanup are observed; the new prompt-exit fix has not yet been installed in this component report.','No RF, keyed failure behavior or actual6300 qualification.','Three unprivileged control processes may remain after resource retirement; this is not a claim of complete cgroup removal.'],
}
report['evidence_sha256']={str(p.relative_to(W)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(E.rglob('*')) if p.is_file()}
f=W/'tools/pd-waveform/PREPARED-CHILD-EXIT-QUALIFICATION.json';tmp=f.with_suffix('.json.tmp');tmp.write_text(json.dumps(report,indent=2)+'\n');tmp.replace(f)
planpath=B/'production-completion-plan-20261113.json';plan=json.loads(planpath.read_text());plan['prepared_child_exit']={'status':report['status'],'report':str(f),'installed_fix':False,'baseline_capture_failure_cleanup_ms':p['workers_gone_ms'],'RF_commands':[]};plan['current_focus']='Install and repeat the bounded capture-startup failure against the prompt child-exit fix; then ordinary completeIQ recovery. Full P1-P10 remains active.'
tmp=planpath.with_suffix('.json.tmp');tmp.write_text(json.dumps(plan,indent=2)+'\n');tmp.replace(planpath)
print({'component_report':str(f),'evidence_files':len(report['evidence_sha256'])})
