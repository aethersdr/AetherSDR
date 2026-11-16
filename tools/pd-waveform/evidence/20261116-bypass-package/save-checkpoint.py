import datetime,hashlib,json,pathlib,shutil,subprocess
p=pathlib.Path(__file__).resolve().parent;e=p.parent;w=e.parent/'aetherpd-source';pd=w/'tools/pd-waveform'
head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=w,text=True).strip()
r=json.loads((e/'runtime-bypass-readiness/report.json').read_text());assert r['source_commit']==head
assert all(x['identical_manifest'] and x['identical_release'] for x in r['architectures'].values())
dest=pd/'evidence/20261116-bypass-package';dest.mkdir(exist_ok=False)
def copy(src,dst):
 dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dst)
for root in ['runtime-bypass-readiness','upgrade-bypass-readiness','bypass-live','bypass-rf']:
 for f in sorted((e/root).glob('*')):
  if f.is_file() and f.suffix in ['.py','.json','.c','.log']:
   copy(f,dest/root/f.name)
for f in sorted((e/'bypass-rf/5w').glob('*.py')):copy(f,dest/'bypass-rf/5w'/f.name)
copy(pathlib.Path(__file__),dest/'save-checkpoint.py')
backup=pathlib.Path('/home/jeremy/AetherSDR/aetherpd-checkpoints/20261116-bypass-readiness');backup.mkdir(parents=True,exist_ok=False)
for arch in ['armv7','arm64']:
 for name in ['candidate.release','manifest.json','public-key','release-digest']:
  copy(e/'runtime-bypass-readiness/short'/arch/name,backup/arch/name)
for name in ['report.json','budget-report.json','source-sha256.json']:
 copy(e/'runtime-bypass-readiness'/name,backup/name)
subprocess.run(['git','archive','--format=tar.gz','--output='+str(backup/'committed-source.tar.gz'),head],cwd=w,check=True)
manifest={str(f.relative_to(dest)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(dest.rglob('*')) if f.is_file()}
artifacts={str(f.relative_to(backup)):{'bytes':f.stat().st_size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in sorted(backup.rglob('*')) if f.is_file()}
report={'schema':1,'updated_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'source_commit':head,'installed':False,'production_ready':False,'status':'Two-path ARMv7 and ARM64 signed runtime builds reproduced byte-for-byte. Upgrade admission size boundaries passed. Deployment scripts prepared but not executed.',
 'runtime_report':r,'budget_report':json.loads((e/'runtime-bypass-readiness/budget-report.json').read_text()),
 'evidence_directory':str(dest.relative_to(pd)),'evidence_sha256':manifest,'durable_artifact_directory':str(backup),'artifact_sha256':artifacts,
 'remaining':['Build complete eight-file upgrade distribution using upgrade-bypass-readiness/build.py.','Build/relaunch desktop with that selection, inspect concrete plan, then install through approved ordinary workflow.','Receive IQ check and cold5W bypass/corrected RF tests; stop/audit and preserve outcomes.','Full P1-P10 goal remains active; actual6300, sustained/fault/rollback, final stock comparison and release gates not complete.'],
 'limits':['Prepared deployment/RF scripts are not executed or qualified by this checkpoint.','Runtime tar and releases contain no private development signing seed; publisher remains in QtKeychain.','ARMv7 capacity is modeled from real package sizes, not measured6300 filesystem usage.','Current installed runtime isba00; this fix is still uninstalled.']}
(pd/'BYPASS-PACKAGE-QUALIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
(backup/'checkpoint.json').write_text(json.dumps(report,indent=2)+'\n')
progress=json.loads((pd/'PROGRESS.json').read_text());progress['updated_utc']=report['updated_utc'];progress['baseline_package']={'report':'BYPASS-PACKAGE-QUALIFICATION.json','runtime_source':head,'two_path_reproducible':True,'installed':False,'durable_artifacts':str(backup)}
progress['next_action']=report['remaining'][0]+' '+report['remaining'][1]+' '+report['remaining'][2]
(pd/'PROGRESS.json').write_text(json.dumps(progress,indent=2)+'\n')
print(json.dumps({'saved_artifacts':len(artifacts),'bytes':sum(x['bytes'] for x in artifacts.values()),'backup':str(backup),'evidence_files':len(manifest)}))
