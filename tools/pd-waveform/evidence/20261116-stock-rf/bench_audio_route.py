"""Temporary synthetic PCM route for only AetherSDR PID1365774; no global defaults."""
import json,os,pathlib,select,subprocess,sys,time
APP_PID='1365774';SINK='aetherpd_bench_input'
def pulse(*args):
 return subprocess.run(['pactl',*map(str,args)],capture_output=True,text=True,timeout=5,check=True).stdout

def inputs():
 clients={str(c['index']):c.get('properties',{}) for c in json.loads(pulse('-f','json','list','clients'))}
 return [(o,clients.get(str(o.get('client')),{})) for o in json.loads(pulse('-f','json','list','source-outputs'))]
def owned_input(index=None):
 found=[o for o,c in inputs() if c.get('application.process.id')==APP_PID and c.get('application.name')=='AetherSDR' and (index is None or o['index']==index)]
 assert len(found)==1,('AetherSDR input identity ambiguous',[(o['index'],o.get('source')) for o in found]);return found[0]
def restore(index,source,module,path):
 result={'input_index':index,'original_source':source,'module':module,'errors':[]}
 try:
  o=owned_input(index);sources=json.loads(pulse('-f','json','list','sources'));ours=next((s['index'] for s in sources if s['name']==SINK+'.monitor'),None)
  if o['source']==ours:pulse('move-source-output',index,source)
  elif o['source']!=source:result['operator_route_change_preserved']=True
  o=owned_input(index);result['final_source']=o['source'];result['source_restored']=o['source']==source
 except BaseException as e:result['errors'].append('input restoration: '+repr(e))
 try:pulse('unload-module',module)
 except BaseException as e:result['errors'].append('module removal: '+repr(e))
 result['module_removed']=not any('sink_name='+SINK in m.get('argument','').split() for m in json.loads(pulse('-f','json','list','modules')))
 pathlib.Path(path).write_text(json.dumps(result,indent=2)+'\n')
 return result

class Route:
 def __init__(self,path):self.path=pathlib.Path(path);self.writer=None;self.watchdog=None;self.index=None;self.source=None;self.module=None
 def start(self):
  assert not any(s['name']==SINK+'.monitor' for s in json.loads(pulse('-f','json','list','sources'))),'bench source already exists'
  o=owned_input();self.index=o['index'];self.source=o['source']
  self.module=int(pulse('load-module','module-null-sink','sink_name='+SINK,'rate=48000','channels=2','channel_map=front-left,front-right','format=float32le').strip())
  reader,self.writer=os.pipe()
  try:
   watch_log=self.path.with_suffix('.watchdog.log').open('w')
   self.watchdog=subprocess.Popen([sys.executable,__file__,'watch',str(reader),str(self.index),str(self.source),str(self.module),str(self.path)],pass_fds=(reader,),start_new_session=True,stdout=watch_log,stderr=subprocess.STDOUT)
   watch_log.close()
  except BaseException:
   os.close(self.writer);self.writer=None;restore(self.index,self.source,self.module,self.path);raise
  finally:os.close(reader)
  deadline=time.monotonic()+3
  while time.monotonic()<deadline:
   if any(s['name']==SINK+'.monitor' for s in json.loads(pulse('-f','json','list','sources'))):break
   time.sleep(.1)
  else:raise AssertionError('temporary monitor did not appear')
  try:pulse('move-source-output',self.index,SINK+'.monitor')
  except subprocess.CalledProcessError as e:raise RuntimeError('move-source-output: '+e.stderr) from e
  time.sleep(.25)
  sources=json.loads(pulse('-f','json','list','sources'));monitor=next(s['index'] for s in sources if s['name']==SINK+'.monitor')
  assert owned_input(self.index)['source']==monitor,'temporary input route not confirmed'
  return {'input_index':self.index,'original_source':self.source,'temporary_monitor':monitor,'module':self.module,'client_pid':APP_PID}
 def close(self):
  if self.writer is not None:os.close(self.writer);self.writer=None
  if self.watchdog is not None:
   self.watchdog.wait(timeout=12);self.watchdog=None
  if self.path.exists():return json.loads(self.path.read_text())
  return {'error':'no restoration report'}

if __name__=='__main__':
 if len(sys.argv)>1 and sys.argv[1]=='watch':
  reader,index,source,module=map(int,sys.argv[2:6]);path=sys.argv[6]
  select.select([reader],[],[],180);os.close(reader);r=restore(index,source,module,path);sys.exit(bool(r['errors']))
 else:
  route=Route('/tmp/aetherpd-6000/out/audio-route-receive-only.json')
  try:print(json.dumps(route.start()));time.sleep(.5)
  finally:print(json.dumps(route.close()))
