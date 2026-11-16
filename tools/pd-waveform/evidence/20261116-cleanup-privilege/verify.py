import hashlib,json,os,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parent;repo=ROOT.parents[1]/'aetherpd-source';source=repo/'tools/pd-waveform/session';env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'zig-cache'),ZIG_LOCAL_CACHE_DIR=str(ROOT/'zig-cache'))
report={'production_installed':False,'builds':{},'mutations':{}}
def run(args,name,environment=None):
 p=subprocess.run(args,env=environment,capture_output=True,text=True);(ROOT/(name+'.log')).write_text(p.stdout+p.stderr);return p
for arch,flags in [('host',[]),('arm64',['-target','aarch64-linux-musl']),('armv7',['-target','arm-linux-musleabihf','-mcpu=cortex_a8'])]:
 output=ROOT/('pd-radio-launch-'+arch);cmd=['zig','cc',*flags,'-O2','-static','-s','-Wall','-Wextra','-Werror','-Wl,--build-id=none','-pthread',str(source/'pd_radio_launch.c'),'-o',str(output)];p=run(cmd,'build-'+arch,env);assert p.returncode==0,(arch,p.stderr);report['builds'][arch]={'bytes':output.stat().st_size,'sha256':hashlib.sha256(output.read_bytes()).hexdigest()}
for name,change in [
 ('no-drop',lambda s:s.replace('if(effective_uid==0 && !retire_privileges()){return 0;}','(void)effective_uid;(void)retire_privileges;')),
 ('ignore-drop-failure',lambda s:s.replace('if(effective_uid==0 && !retire_privileges()){return 0;}','if(effective_uid==0){(void)retire_privileges();}')),
 ('ignore-reap-failure',lambda s:s.replace('return reap(children);','(void)reap(children);return 1;'))]:
 dest=ROOT/name;shutil.copytree(source,dest,dirs_exist_ok=True);p=dest/'pd_radio_launch.c';original=p.read_text();changed=change(original);assert changed!=original;p.write_text(changed);output=ROOT/('test-'+name)
 build=run(['cc','-O2','-Wall','-Wextra','-Werror','-pthread',str(dest/'radio_launch_test.c'),'-o',str(output)],'mutation-build-'+name);assert build.returncode==0,build.stderr
 test=run([str(output)],'mutation-'+name);assert test.returncode!=0,'Mutation survived: '+name;report['mutations'][name]={'exit':test.returncode,'assertion':test.stderr.strip()[-600:]}
output=ROOT/'radio-launch-test-sanitized';p=run(['cc','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',str(source/'radio_launch_test.c'),'-o',str(output)],'sanitizer-build');assert p.returncode==0,p.stderr
e=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1');p=run([str(output)],'sanitizer-run',e);assert p.returncode==0,p.stderr;report['ASan_UBSan_LSan_exit']=p.returncode
(ROOT/'verification.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
