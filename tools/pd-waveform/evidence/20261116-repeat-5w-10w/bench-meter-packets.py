"""Bounded read-only meter subscription for one separately invoked GUI bench run."""
import socket,select,struct,time,json,pathlib
p=pathlib.Path(__file__).resolve().parent;out=p/'meter-packets.json';assert not out.exists()
tcp=socket.create_connection(('192.168.50.100',4992),2);tcp.setblocking(False)
udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind((tcp.getsockname()[0],0));udp.setblocking(False)
port=udp.getsockname()[1];tcp.sendall(f'C1|client udpport {port}\nC2|sub meter all\n'.encode())
start=time.monotonic();rows=[];replies={};buffer=b'';counts={}
try:
 while time.monotonic()-start<30 and not (p/'meter-packets.stop').exists():
  readable,_,_=select.select([tcp,udp],[],[],.1)
  for sock in readable:
   if sock is tcp:
    data=tcp.recv(32768)
    if not data:raise RuntimeError('API subscription closed')
    buffer+=data;assert len(buffer)<262144
    while b'\n' in buffer:
     line,buffer=buffer.split(b'\n',1)
     if line.startswith(b'R'):
      fields=line.split(b'|',2);replies[fields[0][1:].decode()]=int(fields[1],16)
   else:
    data,source=udp.recvfrom(65535)
    if source[0]!='192.168.50.100' or len(data)<28:continue
    header=struct.unpack_from('>I',data)[0];pcc=struct.unpack_from('>I',data,12)[0]&65535
    counts[str(pcc)]=counts.get(str(pcc),0)+1
    if pcc!=0x8002:continue
    end=len(data)-(4 if header&0x04000000 else 0);assert end>=28 and (end-28)%4==0
    values={str(i):v for i,v in struct.iter_unpack('>Hh',data[28:end]) if i in (8,9,10)}
    if values:rows.append({'ns':time.monotonic_ns(),'packet_count':(header>>16)&15,'raw':values})
  if replies.get('1')==0 and replies.get('2')==0 and counts and not (p/'meter-packets.ready').exists():
   (p/'meter-packets.ready').write_text(json.dumps({'port':port,'replies':replies,'counts':counts}))
finally:
 udp.close();tcp.close();out.write_text(json.dumps({'seconds':time.monotonic()-start,'replies':replies,'counts':counts,'rows':rows},indent=2)+'\n')
print('Collected',len(rows),'meter packets.')
