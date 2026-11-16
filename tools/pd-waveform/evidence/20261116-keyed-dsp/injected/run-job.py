import hashlib,subprocess,sys
from pathlib import Path
out=Path(__file__).resolve().parent;case=sys.argv[1];assert case.replace('-','').isalnum();job=out/(case+'-job.py');result=out/(case+'-result.json');assert job.is_file() and not result.exists()
base=Path('/home/jeremy/agent-worktrees/c3957d44/flexipv6-build');package=base/'native/NereusNativeProbe.ssdr_waveform'
cmd=['python3',str(base/'src/operator/native-run.py'),str(job),str(result),'--host','192.168.50.100','--ssh-key',str(base/'keys/probe'),'--known-hosts','/tmp/aetherpd-shim-integration-kh-probe','--token-file',str(base/'native/client-token.txt'),'--native-package',str(package),'--state-dir',str(out/(case+'-state')),'--expected-sha256',hashlib.sha256(package.read_bytes()).hexdigest()]
with (out/(case+'.log')).open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
