"""Retain exact installed-build evidence separately from the uninstalled fix."""
import datetime,hashlib,json,pathlib,re,shutil,subprocess
p=pathlib.Path(__file__).resolve().parent;e=p.parent;w=e.parent/'aetherpd-source';pd=w/'tools/pd-waveform'
def read(f):return json.loads(f.read_text())
def write(f,d):
 t=f.with_suffix(f.suffix+'.new');t.write_text(json.dumps(d,indent=2)+'\n');t.replace(f)
def copy(src,dest):
 dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dest)
def manifest(dest):
 result={}
 for f in sorted(dest.rglob('*')):
  if not f.is_file():continue
  if f.suffix in ('.py','.json','.log'):
   assert not re.search(r'-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----|Bearer [A-Za-z0-9._~-]{16,}|"auth_token"\s*:\s*"[^\"]+"',f.read_text()),str(f)
  result[str(f.relative_to(dest))]=hashlib.sha256(f.read_bytes()).hexdigest()
 return result
now=datetime.datetime.now(datetime.timezone.utc).isoformat();head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=w,text=True).strip();assert head=='ba00b01c43676ac60d4f05ff1e76fae629a02e2e'
install=read(e/'acquisition-live/install-result.json');assert install['committed'] and not install['diagnostics_used_for_delivery']
iq=read(e/'acquisition-live/receive-iq-check.json');assert iq['passed'] and iq['iq_bytes']==131072
stop=read(e/'acquisition-rf/stopped.json');assert stop['passed']
audit=read(e/'guardian-owner-live/acquisition-idle-result.json');host=json.loads(audit['logs']);assert host['passed'] and audit['job_container_deleted'] and audit['temporary_host_directory_deleted']
comparison=read(e/'acquisition-rf/comparison.json')
cases={}
for label in ['off-01','on-01']:
 export=read(e/'acquisition-rf/5w'/(label+'.json'));timing=read(e/'acquisition-rf'/(label+'-timing.json'))
 assert export['measurement']['valid'] and export['measurement']['averages']==4 and export['correction_active']==label.startswith('on-')
 assert timing['released'] and not any(timing['faults_at_end'].values())
 cases[label]={'timing':timing,'valid_measurement':True,'averages':4,'correcting':export['correction_active'],'imd3_per_tone_db':[x['dbc_per_tone'] for x in export['measurement']['products'] if x['order']==3]}
dest=pd/'evidence/20261116-acquisition-installed';dest.mkdir(exist_ok=False)
for root in ['acquisition-rf','acquisition-live']:
 for source in sorted((e/root).rglob('*')):
  if source.is_file() and '__pycache__' not in source.parts:copy(source,dest/root/source.relative_to(e/root))
for root in ['runtime-acquisition-timing','upgrade-acquisition-timing']:
 for source in sorted((e/root).iterdir()):
  if source.suffix in ['.json','.py','.c'] and source.is_file():copy(source,dest/root/source.name)
for suffix in ['-job.py','-result.json']:copy(e/'guardian-owner-live'/('acquisition-idle'+suffix),dest/('acquisition-idle'+suffix))
report={'schema':1,'updated_utc':now,'production_ready':False,'runtime_source':head,'desktop_source':head,'status':'Ordinary signed upgrade, receive IQ, finite 5W bypass/corrected pair and stopped-session audit passed.',
 'installation':{'committed':True,'elapsed_s':install['elapsed_s'],'broad_diagnostics_used_for_delivery':False,'release_blake2b_256':'cbe4f5f6db8cfae2dec8a170efd76dab0b54dee6764858abc2f042e928649cb1','identity_sha256':host['identity_sha256']},
 'radio':{'model':'FLEX-8600','firmware':'4.3.16.42034','frequency_mhz':7.15,'nominal_watts':5,'configured_max_watts':10,'FDX':False,'transport':'local_dax_iq','channel':4,'transmit_antenna':'ANT1','feedback':'RX_A','preamp_db':0},
 'receive_iq_bytes':iq['iq_bytes'],'cases':cases,'comparison':{k:v for k,v in comparison.items() if k!='cases'},
 'independent_recomputation':{k:{'independent':v['independent'],'gui_max_difference_db':v['gui_max_difference_db']} for k,v in comparison['cases'].items()},
 'cleanup':host,'temporary_audit_components_removed':True,
 'findings':['Native alignment was available about2.026s after keying. Estimator work was261–266ms; most pre-result time was collection. This is acquisition time, not FPGA-to-process packet latency.',
 'Host polling first observed ready tones6.338s/6.478s after GUI intent. Baseline readiness also checks local_iq_fault, which remains set until correction_ready despite correction being bypassed. An offline regression reproduces that unnecessary gate; the fix is not in this installed build.',
 'The corrected run initially exposes the previous run\'s retained acquisition. Timing analysis marks records observed before current intent; retained history is not a new reset.',
 'The first status poll after pdStop exited nonzero. A subsequent status-only poll confirmed stopped; the session was not restarted.'],
 'limitations':['Two finite5W transmissions, not sustained speech, broad repeatability,10W or stockAPD qualification.','Historical128sample alignment slip and cold-start cancellation remain unresolved.','No new reboot on this build; prior reboot proof belongs tofa8.','Absolute RF power and ADC headroom uncalibrated; LP100A deferred.','Desktop capture debug log covers on-01 only; off-01 retains native diagnostic timing.','Development publisher and local pins, not public release catalogue.','Actual6300 hardware, interrupted-update/rollback, remaining fault matrix and independent release gates remain open.'],
 'evidence_directory':str(dest.relative_to(pd)),'evidence_sha256':manifest(dest)}
write(pd/'ACQUISITION-INSTALLED-QUALIFICATION.json',report)
fixdest=pd/'evidence/20261116-bypass-readiness';fixdest.mkdir(exist_ok=False)
for name in ['before-fix.log','after-fix-ctest.log','native-sanitizers.json','pd_dispatch_deadline_test-asan.log','pd_calibration_dispatch_test-asan.log','mutations.json','bypass-readiness-mutation.log','cross-builds.json','docs-build.log','check-native.py','cross-build.py','mutation-check.py','save-evidence.py']:
 copy(p/name,fixdest/name)
assert all(v==0 for v in read(p/'native-sanitizers.json').values())
assert read(p/'mutations.json')['bypass-readiness']['exit']!=0
fix={'schema':1,'updated_utc':now,'production_ready':False,'installed':False,'base_commit':head,
 'status':'Baseline capture no longer requires a correction model; offline regression, mutation, three CTests, two sanitizer tests, ARMv7/ARM64 builds and user-docs build passed.',
 'behavior':{'bypass':'Fresh aligned IQ, settled bounded source, valid TX continuity and no native APD required. Missing correction model is allowed.','corrected':'Also requires fault clear and correction_ready; a clear fault alone cannot stand in for a model.','unchanged':'No TX keying, source rearm, correction DSP, authorization deadline or IQ age limit changes.'},
 'source_sha256':{n:hashlib.sha256((w/n).read_bytes()).hexdigest() for n in ['tools/pd-waveform/pd_waveform.c','tools/pd-waveform/calibration_dispatch_test.c','docs/user/docs/tailscale-remote-access.md']},
 'cross_builds':read(p/'cross-builds.json'),'sanitizers':read(p/'native-sanitizers.json'),'mutation':read(p/'mutations.json'),
 'limits':['Not deployed or RF-qualified.','Not a demonstrated resolution of historical128sample slip or all cold-start failures.','Cross-compilation is not actual6300 hardware qualification.','Documentation build used existing node_modules; no publication or CI run.'],
 'evidence_directory':str(fixdest.relative_to(pd)),'evidence_sha256':manifest(fixdest)}
write(pd/'BYPASS-READINESS-QUALIFICATION.json',fix)
progress=read(pd/'PROGRESS.json');progress['updated_utc']=now
progress['baseline_readiness']={'report':'BYPASS-READINESS-QUALIFICATION.json','offline_passed':True,'installed':False}
progress['acquisition_installed']={'report':'ACQUISITION-INSTALLED-QUALIFICATION.json','ordinary_upgrade_passed':True,'finite_5w_pair_passed':True,'session_stopped_and_audited':True}
progress['next_action']='Package and install the baseline-readiness fix through the ordinary desktop workflow; qualify cold5W bypass/corrected captures and retained freshness guards. Continue historical alignment/startup, sustained/fault/recovery, final comparison and actual6300 gates.'
current=progress['current_delivery_status'];current['latest_live_report']='ACQUISITION-INSTALLED-QUALIFICATION.json';current['installed_state']='ba00 desktop/runtime ordinary upgrade, IQ4/FDXoff and finite5W pair passed; session stopped and cleanup audited. Baseline-readiness fix remains offline only.';current['source_provenance_note']='Latest installed/RF buildba00. Reboot proof remainsfa8; earlier10W/stock results keep their original attribution.'
write(pd/'PROGRESS.json',progress)
print(json.dumps({'installed_evidence_files':len(report['evidence_sha256']),'fix_evidence_files':len(fix['evidence_sha256'])}))
