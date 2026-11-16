import hashlib,importlib.util,json,pathlib,shlex,subprocess,sys
P=pathlib.Path;out=P(__file__).resolve().parent;w=P('/home/jeremy/AetherSDR/build/aetherpd-source');b=out.parent/'first-install-desktop-monitor/project'
prepared=out/'prepared'
if '--verify-only' not in sys.argv:
 line=subprocess.check_output(['ninja','-C',str(b),'-t','commands','aetherpd_package_test'],text=True).splitlines()[-1]
 parts=shlex.split(line);libs=parts[parts.index('-o')+2:];assert libs[-2:]==['&&',':'];libs=libs[:-2]
 qt=P(next(s for s in libs if s.endswith('libQt6Core.so.6.12.0'))).parent.parent
 cmd=['/usr/bin/c++','-std=c++20','-fPIC','-I'+str(w/'src'),'-I'+str(qt/'include'),'-I'+str(qt/'include/QtCore'),'-I'+str(out/'short'),str(out/'desktop-prepare.cpp'),'-o',str(out/'desktop-prepare'),*libs]
 (out/'desktop-build-command.json').write_text(json.dumps(cmd,indent=2)+'\n');subprocess.run(cmd,cwd=b,check=True)
 prepared=out/'prepared';prepared.mkdir(exist_ok=False)
 subprocess.run([str(out/'desktop-prepare'),str(prepared)],check=True)
sys.path.insert(0,str(w/'tools/pd-waveform/request'));import first_install_package
native=(prepared/'native.ssdr_waveform').read_bytes();carrier=(prepared/'carrier.tar.gz').read_bytes();plan=(prepared/'plan').read_bytes()
names=first_install_package.verify_uploads(native,carrier,plan)
try:first_install_package.verify_uploads(native,carrier,(prepared/'other-plan').read_bytes())
except (ValueError, KeyError):pass
else:raise AssertionError('mixed attempt accepted')
report={'actual_desktop_archives_verified_by_python':True,'mixed_attempt_rejected':True,'names':names,'native_sha256':hashlib.sha256(native).hexdigest(),'carrier_sha256':hashlib.sha256(carrier).hexdigest(),'uploaded':False}
(out/'desktop-interop.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
