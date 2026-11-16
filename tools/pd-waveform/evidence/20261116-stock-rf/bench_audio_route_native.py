"""Bounded native PipeWire links for a synthetic AetherSDR mic bench source."""
import json,os,pathlib,select,subprocess,sys,time,tempfile
from bench_audio_route import pulse,owned_input,SINK,APP_PID

def atomic_state(path,value):
 fd,name=tempfile.mkstemp(prefix=path.name+'.',dir=path.parent)
 try:
  with os.fdopen(fd,'w') as f:json.dump(value,f,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
  os.replace(name,path)
 finally:
  if os.path.exists(name):os.unlink(name)

def graph():return json.loads(subprocess.check_output(['pw-dump'],text=True,timeout=5))
def props(x):return x.get('info',{}).get('props',{})
def links(g,node):return [x for x in g if x.get('type')=='PipeWire:Interface:Link' and x.get('info',{}).get('input-node-id')==node]
def pw(*args):return subprocess.run(['pw-link',*map(str,args)],capture_output=True,text=True,timeout=5,check=True)
def identity(g,serial):
 found=[x for x in g if str(props(x).get('object.serial'))==str(serial)]
 assert len(found)==1,('PipeWire object missing or changed',serial);return found[0]
def edge(g,link):
 d=link['info'];by={x['id']:x for x in g}
 return {'output_serial':props(by[d['output-port-id']])['object.serial'],'input_serial':props(by[d['input-port-id']])['object.serial']}
def has_edge(g,e):
 o=identity(g,e['output_serial'])['id'];i=identity(g,e['input_serial'])['id']
 return any(x.get('type')=='PipeWire:Interface:Link' and x['info']['output-port-id']==o and x['info']['input-port-id']==i for x in g)
def connect(g,e):pw('-L','-w',identity(g,e['output_serial'])['id'],identity(g,e['input_serial'])['id'])
def disconnect(g,e):pw('-d',identity(g,e['output_serial'])['id'],identity(g,e['input_serial'])['id'])
def restore(state_path,report_path):
 s=json.loads(pathlib.Path(state_path).read_text());r={'errors':[],'module':s['module'],'original_edges':s['original_edges']}
 try:
  owned_input(s['input_index']);g=graph();node=identity(g,s['node_serial']);assert node['id']==s['node_id']
  current=[edge(g,l) for l in links(g,node['id'])];known=s['original_edges']+s.get('temporary_edges',[])
  foreign=[e for e in current if e not in known]
  if foreign:r['operator_route_change_preserved']=True
  for e in s.get('temporary_edges',[]):
   g=graph()
   if has_edge(g,e):disconnect(g,e)
  for e in ([] if foreign else s['original_edges']):
   g=graph()
   if not has_edge(g,e):connect(g,e)
  g=graph();r['original_links_restored']=all(has_edge(g,e) for e in s['original_edges'])
  r['final_capture_edges']=[edge(g,l) for l in links(g,node['id'])]
 except BaseException as e:r['errors'].append('link restoration: '+repr(e))
 try:pulse('unload-module',s['module'])
 except BaseException as e:r['errors'].append('module removal: '+repr(e))
 try:r['module_removed']=not any('sink_name='+SINK in m.get('argument','').split() for m in json.loads(pulse('-f','json','list','modules')))
 except BaseException as e:r['errors'].append('module check: '+repr(e))
 pathlib.Path(report_path).write_text(json.dumps(r,indent=2)+'\n');return r

class Route:
 def __init__(self,path):
  self.path=pathlib.Path(path);self.state_path=self.path.with_suffix('.state.json');self.writer=None;self.watchdog=None;self.state=None
 def start(self):
  assert not any(x['name']==SINK+'.monitor' for x in json.loads(pulse('-f','json','list','sources'))),'bench sink already exists'
  app=owned_input();g=graph();node=identity(g,app['index']);assert props(node).get('media.class')=='Stream/Input/Audio'
  original=links(g,node['id']);assert len(original)==2,'unexpected capture topology'
  s={'input_index':app['index'],'client_pid':APP_PID,'node_id':node['id'],'node_serial':props(node)['object.serial'],'original_edges':[edge(g,l) for l in original]}
  s['module']=int(pulse('load-module','module-null-sink','sink_name='+SINK,'rate=48000','channels=2','channel_map=front-left,front-right','format=float32le').strip());self.state=s
  atomic_state(self.state_path,s)
  reader,self.writer=os.pipe();log=self.path.with_suffix('.watchdog.log').open('w')
  try:self.watchdog=subprocess.Popen([sys.executable,__file__,'watch',str(reader),str(self.state_path),str(self.path)],pass_fds=(reader,),start_new_session=True,stdout=log,stderr=subprocess.STDOUT)
  except BaseException:
   os.close(self.writer);self.writer=None;restore(self.state_path,self.path);raise
  finally:os.close(reader);log.close()
  deadline=time.monotonic()+3
  while time.monotonic()<deadline:
   g=graph();sink=next((x for x in g if x.get('type')=='PipeWire:Interface:Node' and props(x).get('node.name')==SINK),None)
   if sink is not None:
    outputs=[x for x in g if x.get('type')=='PipeWire:Interface:Port' and props(x).get('node.id')==sink['id'] and props(x).get('port.direction')=='out']
    if len(outputs)==2:break
   time.sleep(.1)
  else:raise AssertionError('bench monitor ports unavailable')
  by_id={x['id']:x for x in g};original_ports=[by_id[l['info']['input-port-id']] for l in original]
  target={'FL':'AUX0','FR':'AUX1'};s['temporary_edges']=[]
  for o in outputs:
   i=next(x for x in original_ports if props(x)['audio.channel']==target[props(o)['audio.channel']]);s['temporary_edges'].append({'output_serial':props(o)['object.serial'],'input_serial':props(i)['object.serial']})
  atomic_state(self.state_path,s)
  for e in s['temporary_edges']:connect(graph(),e)
  for e in s['original_edges']:disconnect(graph(),e)
  time.sleep(.3);self.verify();return s
 def verify(self):
  assert self.watchdog and self.watchdog.poll() is None,'audio route lease expired'
  owned_input(self.state['input_index']);g=graph();node=identity(g,self.state['node_serial']);actual=[edge(g,l) for l in links(g,node['id'])]
  assert sorted(actual,key=str)==sorted(self.state['temporary_edges'],key=str),('capture route changed',actual)
 def close(self):
  if self.writer is not None:os.close(self.writer);self.writer=None
  if self.watchdog:self.watchdog.wait(timeout=15);self.watchdog=None
  if self.path.exists():return json.loads(self.path.read_text())
  return {'error':'no restoration report'}

if __name__=='__main__':
 if len(sys.argv)>1 and sys.argv[1]=='watch':
  reader=int(sys.argv[2]);select.select([reader],[],[],180);os.close(reader);r=restore(sys.argv[3],sys.argv[4]);sys.exit(bool(r['errors']))
 else:
  route=Route('/tmp/aetherpd-6000/out/audio-native-route-receive-only.json')
  try:print(json.dumps(route.start()));time.sleep(.5);route.verify()
  finally:print(json.dumps(route.close()))
