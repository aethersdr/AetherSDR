"""Read-only radio observer; one bounded SIGSTOP of our exact desktop incarnation."""
import hashlib,json,os,pathlib,select,signal,socket,sys,time
p=pathlib.Path(__file__).resolve().parent;mode=sys.argv[1];assert mode in ('preflight','stall-01')
config=json.loads((p/'observer-config.json').read_text());pid=config['pid'];proc=pathlib.Path('/proc')/str(pid)
assert int(proc.stat().st_uid)==os.getuid()
fd=os.pidfd_open(pid)
assert int((proc/'stat').read_text().rsplit(')',1)[1].split()[19])==config['start_ticks']
assert hashlib.sha256((proc/'exe').read_bytes()).hexdigest()==config['binary_sha256']
report={'mode':mode,'pid':pid,'start_ticks':config['start_ticks'],'gui_handle':config['gui_handle'],'events':[],'rf_commands':[],'paused':False,'resumed':False};start=time.monotonic();keyed=None;paused=None;resumed=None;state=None;owner=None;api=None
output=p/(mode+'-observer.json');assert not output.exists()
def record(event,**values):
 report['events'].append({'elapsed_s':round(time.monotonic()-start,6),'monotonic_ns':time.monotonic_ns(),'event':event,**values});save()
def save():
 t=output.with_suffix('.tmp');t.write_text(json.dumps(report,indent=2));t.replace(output)
try:
 api=socket.create_connection(('192.168.50.100',4992),3);api.sendall(b'C1|sub tx all\n');api.setblocking(False);buffer=b'';ack=False;idle=False;ready=False
 while time.monotonic()-start<45:
  now=time.monotonic()
  if paused is not None and resumed is None and now-paused>=11:
   signal.pidfd_send_signal(fd,signal.SIGCONT);resumed=now;report['resumed']=True;record('desktop_resumed')
  if ready and mode=='preflight' and now-start>3:break
  if resumed is not None and now-resumed>1:break
  if mode=='stall-01' and keyed is not None and paused is None and now-keyed>=1:
   assert state=='TRANSMITTING' and owner==config['gui_handle'],'Transmission ended before fault injection'
   signal.pidfd_send_signal(fd,signal.SIGSTOP);paused=now;report['paused']=True
   stop_deadline=time.monotonic()+.5
   while (proc/'stat').read_text().rsplit(')',1)[1].split()[0]!='T':
    assert time.monotonic()<stop_deadline,'Desktop stop not confirmed';time.sleep(.005)
   record('desktop_paused',process_state='T')
  if not select.select([api],[],[],.02)[0]:continue
  data=api.recv(16384);assert data,'Observer connection closed';buffer+=data;assert len(buffer)<131072
  while b'\n' in buffer:
   line,buffer=buffer.split(b'\n',1);text=line.decode('ascii').strip()
   if text.startswith('R1|'):assert text=='R1|0|',text;ack=True
   if text.startswith('S') and '|interlock ' in text and '|interlock band ' not in text:
    fields=dict(v.split('=',1) for v in text.split('|',1)[1].split()[1:] if '=' in v)
    if all(k in fields for k in ['state','source','tx_client_handle','tx_allowed','reason']):
     record('interlock',line=text,desktop_paused=paused is not None and resumed is None,desktop_process_state=(proc/'stat').read_text().rsplit(')',1)[1].split()[0])
     state=fields['state'];owner=int(fields['tx_client_handle'],16)
     if not ready:idle=state=='READY' and owner==0 and not fields['source'] and fields['tx_allowed']=='1' and not fields['reason']
     elif state=='TRANSMITTING':
      assert mode=='stall-01','Unexpected transmission in receive-only preflight'
      assert owner==config['gui_handle'] and fields['source']=='SW','Unexpected TX owner or source'
      if keyed is None:keyed=time.monotonic();record('first_owned_transmitting')
   if not ready and ack and idle:
    ready=True;(p/(mode+'-observer-ready.json')).write_text(json.dumps({'ready':True,'observer_pid':os.getpid()}));record('observer_ready')
 report['completed']=True
except BaseException as e:report['error']=repr(e)
finally:
 if report['paused'] and not report['resumed']:
  signal.pidfd_send_signal(fd,signal.SIGCONT);report['resumed']=True;record('desktop_resumed_in_finally')
 if api:api.close()
 os.close(fd);report['elapsed_s']=round(time.monotonic()-start,3);save()
print(json.dumps({k:v for k,v in report.items() if k!='events'}),flush=True)
if report.get('error'):raise SystemExit(1)
