"""Save stock comparison provenance, including unmatched and inactive attempts."""
import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil

p=Path(__file__).resolve().parent; pd=p.parents[1]/'aetherpd-source/tools/pd-waveform'
dest=pd/'evidence/20261116-stock-rf'
read=lambda path:json.loads(path.read_text())
comparison=read(p/'comparison.json')
assert comparison['stock_pair_power_match_within_0_5db'] and comparison['corrected_comparison_power_match_within_0_5db']
assert read(p/'stopped-after-rf.json')['status']['body']['state']=='stopped'
audit=read(p.parent/'guardian-owner-live/stock-rf-idle-result.json'); host=json.loads(audit['logs'])
assert host['passed'] and audit['job_container_deleted'] and audit['temporary_host_directory_deleted']
cases={}
for name in ('stock-off-01','stock-off-02','stock-on-02','stock-on-03','stock-off-03'):
    r=read(p/name/'result.json')
    assert r['idle_verified'] and not r['cleanup_errors'] and len(r['key_intents'])==1
    assert len(r['captures'])==4 and r['guard_final']['state']=='ended'
    assert not r['after']['radio']['fullDuplex'] and not r['after']['radio']['transmitting']
    assert r['route_cleanup']['module_removed'] and r['route_cleanup']['original_links_restored']
    assert r['native_after']['input_limited_samples']==0 and all(v==0 for v in r['native_after']['faults'].values())
    cases[name]={'accepted':r.get('completed',False),'error':r.get('error'),
                 'PC_gain':r['source']['PC_gain'],'speech_prelude_seconds':r.get('speech_prelude',{}).get('seconds',0),
                 'GUI_stop_s':r['GUI_stop_s'],'source_seconds':8,'native_cleanup_after_ms':9000,
                 'stock_state_before_key':r['observer_before']['apd'],
                 'equalizer_active_at_frame_completion':[f['stock_apd'].get('equalizer_active') for f in r['captures']],
                 'idle_verified':True,'finite_audio_route_removed':True,'native_faults_at_completion':r['native_after']['faults']}
assert not cases['stock-on-02']['accepted'] and cases['stock-on-02']['equalizer_active_at_frame_completion']==['0']*4
assert cases['stock-on-03']['stock_state_before_key']['equalizer_active']=='1'
dest.mkdir(parents=True,exist_ok=False)
names=['bench-common.py','bench-api.cpp','api-build-command.json','bench_audio_route.py','bench_audio_route_native.py',
       'prepare-rf.py','rf-prepared.json','batch-receive.f32le','run-stock.py','compare-stock.py','comparison.json',
       'analyze-raw.cpp','check-analyzer.py','analyzer-check.json','route-preflight.json','route-preflight.state.json',
       'route-preflight.watchdog.log','stopped-after-rf.json','stock-reference-notes.json','save-rf-evidence.py']
for name in names:shutil.copyfile(p/name,dest/name)
for name in cases:
    target=dest/name;target.mkdir()
    for source in (p/name).iterdir():
        if source.is_file() and source.suffix in ('.json','.py','.log'):
            shutil.copyfile(source,target/source.name)
    if (p/name/'four-frames.f32le').exists():shutil.copyfile(p/name/'four-frames.f32le',target/'four-frames.f32le')
for name in ('stock-rf-idle-job.py','stock-rf-idle-result.json'):shutil.copyfile(p.parent/'guardian-owner-live'/name,dest/name)
speech=p.parent/'processed-speech-rf/speech-input'
for name in ('pcm-manifest.json','source-manifest.json'):shutil.copyfile(speech/name,dest/('speech-'+name))
(dest/'speech-first-three-seconds.f32le').write_bytes((speech/'speech-loop.f32').read_bytes()[:3*48000*2*4])
shutil.copyfile(p.parent/'recovery-qualification/VOICE-LICENSE.txt',dest/'VOICE-LICENSE.txt')
manifest={}
for f in sorted(dest.rglob('*')):
    if not f.is_file():continue
    if f.suffix in ('.json','.py','.cpp','.log'):
        assert not re.search(r'-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----|Bearer [A-Za-z0-9._~-]{16,}|"auth_token"\s*:\s*"[^\"]+"',f.read_text()),str(f)
    manifest[str(f.relative_to(dest))]=hashlib.sha256(f.read_bytes()).hexdigest()
now=datetime.datetime.now(datetime.timezone.utc).isoformat()
report={'schema':1,'updated_utc':now,'production_ready':False,
        'status':'Finite current-candidate stock APD comparison passed at approximately matched output, with limits',
        'runtime_source':'e8d68ad4bcfca1bf87f1d95b6c613cda17fac097','desktop_source':'b1256a1657319360279029cabfcbcb647728b86a',
        'radio':{'model':'FLEX-8600','firmware':'4.3.16.42034','frequency_mhz':7.15,'nominal_watts':10,'configured_max_watts':10,'FDX':False,
                 'tx_antenna':'ANT1','feedback':'RX_A','preamp_db':0,'feedback_transport':'local_dax_iq','IQ_channel':4},
        'cases':cases,'comparison':comparison,
        'matched_pair':{'stock_on':'stock-on-03','stock_off':'stock-off-03','capture_order':'ON then OFF',
                        'stock_source':'same three-second speech prelude then 710/1900 Hz tones; PC gain 80',
                        'aether_source':'on-radio native bounded measurement/calibration source; four late tone frames',
                        'stock_model':'warm: equalizer_active=1 and ACTIVE already observed before key; no reset',
                        'analyzer':'same production C++ BH7, 16384 samples/frame, four frames, 48 kHz; NumPy independently recomputed'},
        'interpretation':{'imd3_lower':'Stock was 1.47 dB lower in this pair','imd3_upper':'AetherPD was 6.11 dB lower in this pair',
                          'imd5':'AetherPD was 2.02/8.55 dB lower','imd7':'AetherPD was 16.47/13.32 dB lower',
                          'overall':'No universal winner: one finite warm comparison at different excitation histories and a 0.475 dB corrected fundamental-power difference.'},
        'negative_evidence':['stock-off-01 drive check was about 7 dB below target and is not used as the matched baseline.',
                             'stock-on-02 remained IDLE/equalizer_active=0 during all retained tone frames; never counted as corrected.',
                             'Stock was already ACTIVE before stock-on-03. The speech prelude did not demonstrate the cause or timing of activation; no cold-start success claim.'],
        'cleanup':{'radio_idle':True,'stock_APD_off':True,'native_session_stopped':True,'ARP_absent':not host['arp'],
                   'accept_local':host['accept_local'],'runtime_hashes_verified':True,'remaining_processes':host['processes'],
                   'temporary_audit_removed':True,'temporary_audio_routes_removed':True,'processing_settings_restored':False},
        'limits':['One finite pair, not sustained speech, repeatability, all-band performance or actual6300 qualification.',
                  'Stock corrected fundamental level was 0.475 dB higher than AetherPD, within the declared 0.5 dB matching criterion but not identical power.',
                  'Raw-IQ ratios do not calibrate RF watts or ADC headroom. LP-100A remains deferred.',
                  'Native source/PC-audio paths and calibration history differ; this comparison measures the complete tested systems.',
                  'Observed GUI stop durations are not independently measured physical RF cessation times.',
                  'All remaining P1-P10 release, historical alignment/cold-start, process-failure, sustained, installation/ARMv7/6300 and independent-review gates remain.'],
        'evidence_directory':str(dest.relative_to(pd)),'evidence_sha256':manifest}
def save(path,value):
    tmp=path.with_suffix('.json.new');tmp.write_text(json.dumps(value,indent=2)+'\n');tmp.replace(path)
save(pd/'STOCK-CANDIDATE-QUALIFICATION.json',report)
progress=read(pd/'PROGRESS.json');progress['updated_utc']=now
progress['current_focus']='Finite current-candidate stock APD comparison completed at 10 W setting, local IQ4, FDX off, approximately matched output. Warm stock model; negative inactive/under-driven attempts retained. Radio idle, temporary sessions/routes removed.'
progress['next_action']='Continue sustained/speech and final-process failure qualification, investigate historical alignment and cold-start failures, then finish installation/ARMv7/actual6300 and independent release gates. Finite stock comparison is documented with limits; do not treat it as sustained or universal superiority.'
progress['stock_candidate']={'report':'STOCK-CANDIDATE-QUALIFICATION.json','finite_RF_comparison_complete':True,'sustained_qualification_complete':False,'production_ready':False}
delivery=progress['current_delivery_status'];delivery['latest_live_report']='STOCK-CANDIDATE-QUALIFICATION.json'
delivery['installed_state']='Runtime e8d68ad4b with desktop b1256a165; finite 5/10 W pairs and stock APD comparison passed with stated limits. Native session stopped and cleanup audited.'
delivery['source_provenance_note']='Current RF reports attribute runtime e8d68ad4b and desktop b1256a165. Stock comparison uses the same installed local IQ capture path and current production analyzer; old reports remain historical evidence.'
delivery['pending_gates']=[('Repeated/sustained exact-candidate tone and speech qualification; finite 5/10 W pairs and current stock comparison are documented' if 'Repeated exact-candidate 5/10 W tone/speech and matched stock APD comparison'==v else v) for v in delivery['pending_gates']]
progress['remaining']=[('Complete sustained/repeated 8600 RF and speech qualification; finite current-candidate stock APD comparison passed with limits.' if v=='Repeat matched8600 RF measurements and compare final candidate with stock APD.' else v) for v in progress['remaining']]
save(pd/'PROGRESS.json',progress)
print(json.dumps({'report':'STOCK-CANDIDATE-QUALIFICATION.json','files':len(manifest),'bytes':sum(f.stat().st_size for f in dest.rglob('*') if f.is_file())}))
