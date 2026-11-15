"""Exercise the actual unprivileged helper against a loopback fake radio, never RF."""
import ctypes
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import time

binary=Path(sys.argv[1]).resolve()
def drop_caps():
 class Header(ctypes.Structure):
  _fields_=[('version',ctypes.c_uint32),('pid',ctypes.c_int)]
 class Caps(ctypes.Structure):
  _fields_=[('effective',ctypes.c_uint32),('permitted',ctypes.c_uint32),('inheritable',ctypes.c_uint32)]
 h=Header(0x20080522,0);caps=(Caps*2)();libc=ctypes.CDLL(None,use_errno=True)
 if libc.capset(ctypes.byref(h),ctypes.byref(caps))!=0:os._exit(99)

ready=b'S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1\n'
def run_case(name,status,expected,unkey=False,reject=False,unkey_reply=b'R910003|0|\n',stdout_loss=False,duration=None,ends_early=False,refresh_status=None,partial_after_ack=False,close_life=False):
 with socket.socket() as server:
  server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);server.bind(('127.0.0.1',4992));server.listen(1)
  api=socket.create_connection(server.getsockname());peer,_=server.accept();peer.settimeout(3)
  read,write=os.pipe();proc=None
  try:
   command=[str(binary),str(api.fileno()),str(read),'0x00001234','0x00005678']
   if duration is not None:command.append(str(duration))
   proc=subprocess.Popen(command,pass_fds=(api.fileno(),read),preexec_fn=drop_caps,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
   api.close();os.close(read);read=None
   wire=peer.makefile('rb');commands=[]
   line=wire.readline();commands.append(line);assert line==b'C910001|sub tx all\n',(line,proc.communicate(timeout=3),proc.returncode,os.getuid(),os.geteuid())
   peer.sendall(ready+b'R910000|0|\nR910001|0|\n')
   assert select.select([proc.stdout],[],[],3)[0],'helper did not become ready'
   first=json.loads(proc.stdout.readline());assert first['event']==('bounded_ready' if duration is not None else 'ready')
   if duration is not None:assert int(first['cleanup_due_ms'])>0
   if stdout_loss:proc.stdout.close();proc.stdout=None
   # Buffered bytes must not turn this lifetime pipe into a command channel.
   os.write(write,b'queued bytes');assert not select.select([peer],[],[],.05)[0]
   if duration is not None:peer.sendall(status)
   if duration is None or close_life:os.close(write);write=None
   if not ends_early:
    line=wire.readline();commands.append(line);assert line==b'C910002|sub tx all\n'
    if duration is not None and not close_life:
     lateness_ms=time.monotonic()*1000-int(first['cleanup_due_ms'])
     assert -5<=lateness_ms<1500,(name,'deadline not enforced with lifetime writer alive',lateness_ms)
     print(name,'cleanup request lateness_ms',round(lateness_ms,3))
    if duration is None:peer.sendall(status)
    elif refresh_status is not None:peer.sendall(refresh_status)
    assert not select.select([peer],[],[],.05)[0],'acted before snapshot ACK'
    acknowledgment=b'R910002|1|\n' if reject else b'R910002|0|\n'
    if partial_after_ack:
     peer.sendall(acknowledgment+b'S0|interlock tx_client_handle=')
     assert not select.select([peer],[],[],.1)[0],'acted with a partial ownership update after ACK'
     peer.sendall(b'0x00009999 state=TRANSMITTING reason= source=SW tx_allowed=1\n')
    else:peer.sendall(acknowledgment)
   if unkey:
    line=wire.readline();commands.append(line);assert line==b'C910003|xmit 0\n'
    if unkey_reply is not None:peer.sendall(unkey_reply)
   output,error=proc.communicate(timeout=4);assert proc.returncode==expected,(name,proc.returncode,output,error)
   events=[first]+[json.loads(v) for v in (output or b'').splitlines()]
   assert not error and all(not v['keys_transmitter'] for v in events)
   assert all(v in [b'C910001|sub tx all\n',b'C910002|sub tx all\n',b'C910003|xmit 0\n'] for v in commands)
   if not unkey:assert not any(v['event'].startswith('unkey_requested') for v in events)
   assert wire.read()==b'', 'helper sent an unexpected trailing command'
   print(name,proc.returncode,[v['event'] for v in events])
  finally:
   if read is not None:os.close(read)
   if write is not None:os.close(write)
   if proc is not None and proc.poll() is None:proc.kill();proc.communicate()
   api.close();peer.close()

run_case('owned software TX',b'S1234|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=SW tx_allowed=1\n',0,unkey=True)
run_case('already idle, no replay',b'',0)
run_case('other GUI TX',b'S0|interlock tx_client_handle=0x00009999 state=TRANSMITTING reason= source=SW tx_allowed=1\n',4)
run_case('hardware PTT',b'S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=MIC tx_allowed=1\n',4)
run_case('query rejected',ready,5,reject=True)
run_case('incomplete safety status',b'S0|interlock state=TRANSMITTING source=SW\n',5)
run_case('unkey ACK rejected',b'S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=SW tx_allowed=1\n',5,unkey=True,unkey_reply=b'R910003|1|\n')
run_case('unkey ACK missing',b'S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=SW tx_allowed=1\n',5,unkey=True,unkey_reply=None)
run_case('Go stdout reader lost',b'S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=SW tx_allowed=1\n',0,unkey=True,stdout_loss=True)
owned=b'S0|interlock tx_client_handle=0x00005678 state=TRANSMITTING reason= source=SW tx_allowed=1\n'
foreign=b'S0|interlock tx_client_handle=0x00009999 state=TRANSMITTING reason= source=SW tx_allowed=1\n'
run_case('deadline with owner alive',owned,0,unkey=True,duration=250)
run_case('deadline without keying',ready,0,duration=250)
run_case('deadline stdout reader lost',owned,0,unkey=True,duration=250,stdout_loss=True)
run_case('bounded owner lifetime ended',owned,0,unkey=True,duration=1000,close_life=True)
run_case('bounded foreign owner',foreign,0,duration=250,ends_early=True)
run_case('bounded completed and rekeyed cycle',owned+ready+owned,0,duration=250,ends_early=True)
run_case('bounded cycle ended during barrier',owned,0,duration=250,refresh_status=ready+owned)
run_case('partial update after ACK',owned,4,partial_after_ack=True)
run_case('bounded partial update after ACK',owned,0,duration=250,partial_after_ack=True)
print('All fake-radio cases passed; no real radio connection or RF.')
