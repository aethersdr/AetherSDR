import concurrent.futures,hashlib,json,os,pathlib,shutil,subprocess
R=pathlib.Path(__file__).resolve().parent;W=R.parents[1]/'aetherpd-source';S=W/'tools/pd-waveform/session';report={'installed':False,'builds':{},'mutations':{}}
def run(cmd,name,env=None,timeout=60):
 try:
  p=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=timeout);(R/(name+'.log')).write_text(p.stdout+p.stderr);return p
 except subprocess.TimeoutExpired as e:
  (R/(name+'.log')).write_text((e.stdout or b'').decode() if isinstance(e.stdout,bytes) else e.stdout or '');raise

def build(item):
 arch,flags=item;target=R/('pd-radio-launch-'+arch);env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(R/('cache-'+arch)),ZIG_LOCAL_CACHE_DIR=str(R/('cache-'+arch)))
 p=run(['zig','cc',*flags,'-O2','-static','-s','-Wall','-Wextra','-Werror','-Wl,--build-id=none','-pthread',str(S/'pd_radio_launch.c'),'-o',str(target)],'build-'+arch,env);assert p.returncode==0,(arch,p.stderr)
 return arch,{'bytes':target.stat().st_size,'sha256':hashlib.sha256(target.read_bytes()).hexdigest()}
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
 for arch,result in pool.map(build,[('host',[]),('arm64',['-target','aarch64-linux-musl']),('armv7',['-target','arm-linux-musleabihf','-mcpu=cortex_a8'])]):report['builds'][arch]=result
p=run(['cc','-O2','-Wall','-Wextra','-Werror','-pthread',str(S/'radio_launch_test.c'),'-o',str(R/'policy-test')],'host-test-build');assert not p.returncode,p.stderr
p=run([str(R/'policy-test')],'host-test');assert not p.returncode,p.stderr;report['host_policy_passed']=True
for name,edit in [
 ('no-exit-observation',lambda s:s.replace('        if(!prepared_child_running(child,waitid)){return 0;}\n','')),
 ('reap-during-observation',lambda s:s.replace('WEXITED|WNOHANG|WNOWAIT','WEXITED|WNOHANG')),
 ('signal-before-ownership',lambda s:s.replace('    int status;pid_t observed;\n    do{observed=waitpid(child,&status,WNOHANG);}while(observed<0 && errno==EINTR);\n    if(observed!=0){return;}\n','')),
]:
 dest=R/name;shutil.copytree(S,dest,dirs_exist_ok=True);path=dest/'pd_prepared_launch.c';old=path.read_text();new=edit(old);assert new!=old;path.write_text(new);exe=R/('test-'+name)
 p=run(['cc','-O2','-Wall','-Wextra','-Werror','-pthread',str(dest/'radio_launch_test.c'),'-o',str(exe)],'mutation-build-'+name);assert not p.returncode,p.stderr
 try:
  p=run([str(exe)],'mutation-'+name,timeout=4);assert p.returncode!=0,('Surviving mutant',name);report['mutations'][name]={'exit':p.returncode,'output':p.stderr[-700:]}
 except subprocess.TimeoutExpired:report['mutations'][name]={'timeout_s':4,'expected':'unobserved exited child leaves readiness waiting for its10second deadline'}
p=run(['cc','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',str(S/'radio_launch_test.c'),'-o',str(R/'policy-sanitized')],'sanitizer-build');assert not p.returncode,p.stderr
p=run([str(R/'policy-sanitized')],'sanitizer-run',dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1'));assert not p.returncode,p.stderr;report['ASan_UBSan_LSan_passed']=True
(R/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
