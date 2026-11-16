import subprocess,shlex,os
from pathlib import Path
out=Path(__file__).resolve().parent;root=Path('/home/jeremy/AetherSDR/build/aetherpd-source')
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Core'],text=True))
base=['c++','-std=c++20','-fPIC','-O1','-g','-Isrc']
command=base+['-Wall','-Wextra','-Werror',str(out/'qt-binding-fixture.cpp'),'src/core/AetherPDProtocol.cpp','-o',str(out/'qt-binding-fixture'),*flags]
subprocess.run(command,cwd=root,check=True)
with (out/'native-go-qt-interop.log').open('w') as log:subprocess.run([str(out/'qt-binding-fixture'),str(out/'public')],stdout=log,stderr=subprocess.STDOUT,check=True)
command=base+['-fsanitize=address,undefined','-fno-omit-frame-pointer','-DAETHERPD_TESTDATA_DIR="'+str(root/'tools/flex-tailnet-shim/testdata/aetherpd/public')+'"','tests/aetherpd_protocol_test.cpp','src/core/AetherPDProtocol.cpp','-o',str(out/'protocol-asan'),*flags]
subprocess.run(command,cwd=root,check=True)
with (out/'protocol-asan.log').open('w') as log:
 r=subprocess.run([str(out/'protocol-asan')],stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
print('protocol sanitizer exit',r.returncode);raise SystemExit(r.returncode)
