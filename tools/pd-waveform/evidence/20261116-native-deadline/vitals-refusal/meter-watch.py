"""Own receive-only API/UDP subscription; no GUI binding or transmit commands."""
import json,select,socket,struct,time
class MeterWatch:
 def __init__(self):
  self.tcp=socket.create_connection(('192.168.50.100',4992),2);self.tcp.setblocking(False)
  self.udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);self.udp.bind((self.tcp.getsockname()[0],0));self.udp.setblocking(False)
  self.buffer=b'';self.replies={};self.latest={};self.rows=[];self.port=self.udp.getsockname()[1]
  self.tcp.sendall(f'C1|client udpport {self.port}\nC2|sub meter all\n'.encode())
 def pump(self):
  end=time.monotonic()+.008
  while time.monotonic()<end:
   readable,_,_=select.select([self.tcp,self.udp],[],[],.001)
   for s in readable:
    if s is self.tcp:
     data=s.recv(32768);assert data,'Meter API disconnected';self.buffer+=data;assert len(self.buffer)<262144
     while b'\n' in self.buffer:
      line,self.buffer=self.buffer.split(b'\n',1)
      if line.startswith(b'R'):
       f=line.split(b'|',2);self.replies[int(f[0][1:])]=int(f[1],16)
    else:
     data,source=s.recvfrom(65535)
     if source[0]!='192.168.50.100' or len(data)<28:continue
     h=struct.unpack_from('>I',data)[0];pcc=struct.unpack_from('>I',data,12)[0]&65535
     if pcc!=0x8002:continue
     size=len(data)-(4 if h&0x04000000 else 0);assert size>=28 and (size-28)%4==0 and (h&65535)*4==len(data)
     raw={str(i):v for i,v in struct.iter_unpack('>Hh',data[28:size]) if i in (4,8,9,10,11)}
     if raw:
      stamp=time.monotonic_ns();self.rows.append({'ns':stamp,'packet_count':(h>>16)&15,'raw':raw});assert len(self.rows)<16384
      for i,v in raw.items():self.latest[i]=(stamp,v)
 def snapshot(self):
  self.pump();now=time.monotonic_ns();return {'age_ms':{i:(now-t)/1e6 for i,(t,v) in self.latest.items()},'raw':{i:v for i,(t,v) in self.latest.items()},'replies':self.replies}
 def ready(self):
  self.pump();return self.replies.get(1)==0 and self.replies.get(2)==0 and all(i in self.latest for i in ('4','8','9','11'))
 def check(self,key_ns):
  self.pump();now=time.monotonic_ns();out={}
  for i,scale,name in [('4',256,'volts'),('8',128,'fwd_dbm'),('9',128,'ref_dbm'),('11',64,'temp_c')]:
   assert i in self.latest,('Required meter absent',i)
   stamp,value=self.latest[i];assert now-stamp<=2_000_000_000,('Meter stale',i);out[name]=value/scale
  out['watts']=10**((out['fwd_dbm']-30)/10);assert out['watts']<=10,('Power guard',out)
  assert out['volts']>=11 and out['temp_c']<=75,('Vitals guard',out)
  if out['watts']>.1 and now-key_ns>=2_000_000_000:
   assert '10' in self.latest and now-self.latest['10'][0]<=2_000_000_000,'SWR absent/stale'
   out['swr']=self.latest['10'][1]/128;assert 1<=out['swr']<=2,('SWR guard',out)
  return out
 def close(self):self.udp.close();self.tcp.close()
