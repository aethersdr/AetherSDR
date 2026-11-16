"""Fixed-process SIGCONT deadline; never keys or restarts an application."""
import json,os,pathlib,select,signal,sys,time
fd,cancel=int(sys.argv[1]),int(sys.argv[2]);deadline=float(sys.argv[3]);out=pathlib.Path(sys.argv[4])
ready=select.select([cancel],[],[],max(0,deadline-time.monotonic()))[0]
action='canceled'
if not ready:
 action='resumed_at_deadline'
 try:signal.pidfd_send_signal(fd,signal.SIGCONT)
 except ProcessLookupError:action='target_gone'
out.write_text(json.dumps({'action':action,'monotonic_ns':time.monotonic_ns()})+'\n')
