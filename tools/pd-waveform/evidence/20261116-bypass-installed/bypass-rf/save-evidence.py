import datetime,hashlib,json,pathlib,re,shutil
p=pathlib.Path(__file__).resolve().parent;e=p.parent;w=e.parent/'aetherpd-source';pd=w/'tools/pd-waveform'
def read(f):return json.loads(f.read_text())
def write(f,x):
 t=f.with_suffix(f.suffix+'.new');t.write_text(json.dumps(x,indent=2)+'\n');t.replace(f)
def copy(f,g):g.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,g)
audit=read(e/'guardian-owner-live/bypass-idle-result.json');host=json.loads(audit['logs']);assert host['passed'] and audit['job_container_deleted'] and audit['temporary_host_directory_deleted']
install=read(e/'bypass-live/install-result.json');iq=read(e/'bypass-live/receive-iq-check.json');assert install['committed'] and iq['passed'] and read(p/'stopped.json')['passed']
baseline=read(p/'5w/off-01.json');failed=read(p/'5w/on-01-failed.json');assert baseline['measurement']['valid'] and baseline['measurement']['averages']==4 and not baseline['correction_active'];assert failed['error']=='IQ capture request failed' and 'measurement' not in failed
cases={}
for label in ['off-01','on-01']:
 t=read(p/(label+'-timing.json'));assert t['released'] and not any(t['faults_at_end'].values());cases[label]=t
assert 'reason=source_samples_complete' in cases['on-01']['diagnostics_at_end']['source_end']['text']
dest=pd/'evidence/20261116-bypass-installed';dest.mkdir(exist_ok=False)
for root in ['bypass-live','bypass-rf']:
 for f in sorted((e/root).rglob('*')):
  if f.is_file() and '__pycache__' not in f.parts:copy(f,dest/root/f.relative_to(e/root))
for f in sorted((e/'upgrade-bypass-readiness').iterdir()):
 if f.is_file() and f.suffix in ['.json','.py','.log']:copy(f,dest/'upgrade-bypass-readiness'/f.name)
for suffix in ['-job.py','-result.json']:copy(e/'guardian-owner-live'/('bypass-idle'+suffix),dest/('bypass-idle'+suffix))
manifest={}
for f in sorted(dest.rglob('*')):
 if not f.is_file():continue
 if f.suffix in ['.json','.py','.log']:
  assert not re.search(r'-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----|Bearer [A-Za-z0-9._~-]{16,}|"auth_token"\s*:\s*"[^\"]+"',f.read_text()),str(f)
 manifest[str(f.relative_to(dest))]=hashlib.sha256(f.read_bytes()).hexdigest()
now=datetime.datetime.now(datetime.timezone.utc).isoformat();provenance=read(e/'upgrade-bypass-readiness/desktop-provenance.json')
report={'schema':1,'updated_utc':now,'production_ready':False,'runtime_source':provenance['runtime_source'],'desktop_source':provenance['source_commit'],'release_blake2b_256':provenance['release_blake2b_256'],
 'status':'Ordinary update and IQ passed; four-frame baseline passed, corrected measurement failed when its third capture overlapped normal source expiry. Session stopped and cleanup audited.',
 'installation':{'committed':True,'seconds':install['elapsed_s'],'diagnostics_used_for_delivery':False,'identity_sha256':host['identity_sha256']},'receive_iq':{'bytes':iq['iq_bytes'],'sha256':iq['iq_sha256'],'FDX':False,'channel':4},
 'bounds':{'positive_key_actions':2,'automatic_retries':0,'frequency_mhz':7.15,'nominal_watts':5,'max_configured_watts':10,'measured_abort_watts':10,'source_seconds':8,'independent_unkey_seconds':9,'desktop_authorization_seconds':10,'FDX':False,'tx_antenna':'ANT1','feedback':'RX_A','preamp_db':0},
 'cases':cases,'baseline':{'averages':4,'valid':True,'imd3_per_tone_db':[x['dbc_per_tone'] for x in baseline['measurement']['products'] if x['order']==3]},
 'corrected':{'valid':False,'error':failed['error'],'completed_frames_in_transport_log':2,'third_capture_http':503,'partial_raw_iq_retained':False,'comparison_available':False},
 'cleanup':host,'audit_components_removed':True,
 'findings':['The uncorrected readiness gate is removed in this exact installed binary. Offline regression proves it no longer requires a model. This first live baseline completed four frames; host-observed ready tones5.709s versus prior6.338s are observations, not an isolated causal timing benchmark.',
 'Corrected alignment completed2.025s after keying with256.91ms worker time. The host first observed ready tones6.996s after GUI intent. No new alignment reset or transport fault was recorded.',
 'Corrected desktop capture log: first frame completed at capture_ms6723; second frame took684ms; third begin request returned503 after349ms. Native source ended normally at its8second sample budget. Third snapshot overlapped source completion, so no valid corrected four-average result exists.',
 'The desktop waits for all chunks before beginning each subsequent frame. On-radio capture sequencing needs to avoid this dependency on desktop/network delay; simply lengthening a capture timeout cannot extend already-ended source samples.',
 'Invalid export contains an error and no measurement object or partial captures. The first export validator assumed an invalid measurement object and raised KeyError after the file was saved. Validator was corrected; no re-export or RF retry occurred.'],
 'next_work':['Design and implement bounded on-radio multi-frame acquisition with immutable source, binding, correction and freshness metadata; preserve TTL, memory bounds and cancellation.', 'Retrieve completed snapshots independently of desktop transfer latency while retaining operator-controlled RF deadlines and no automatic rearm.', 'Requalify cold/warm four-frame captures and correction after protocol/native/shim/desktop tests.'],
 'limitations':['One successful5Wbaseline and one failed corrected run; no new valid OFF/ON comparison.','No new reboot,10W,stockAPD,speech or sustained-duty qualification.','Historical128sample slip and unexplained cancellation remain unresolved.','No actual6300 or independent release qualification; LP100A deferred.'],
 'evidence_directory':str(dest.relative_to(pd)),'evidence_sha256':manifest}
write(pd/'BYPASS-INSTALLED-QUALIFICATION.json',report)
progress=read(pd/'PROGRESS.json');progress['updated_utc']=now;progress['baseline_readiness']['installed']=True
progress['baseline_installed']={'report':'BYPASS-INSTALLED-QUALIFICATION.json','baseline_passed':True,'corrected_capture_passed':False,'session_stopped_and_audited':True}
progress['current_delivery_status']['latest_live_report']='BYPASS-INSTALLED-QUALIFICATION.json'
progress['current_delivery_status']['installed_state']='dd33027runtime/db6cb9desktop: ordinary update and IQ passed.5Wbaseline valid; corrected four-frame capture failed at normal source expiry. Stopped and audited.'
progress['current_delivery_status']['source_provenance_note']='Installed baseline fix isdd33027. Last valid OFF/ONpair remainsba00; reboot proof remainsfa8; historical10W/stock comparisons retain their original attribution.'
progress['next_action']='Implement bounded on-radio multi-frame capture to remove per-frame desktop-transfer delays; preserve RF cutoffs, source identity, cancellation and memory limits. Qualify exact package through native/shim/desktop and cold/warm RF, then continue full P1-P10 gates.'
write(pd/'PROGRESS.json',progress)
print(json.dumps({'report':str(pd/'BYPASS-INSTALLED-QUALIFICATION.json'),'evidence_files':len(manifest)}))
