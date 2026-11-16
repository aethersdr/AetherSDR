"""Receive-only check: delayed userspace draining retains kernel arrival age."""
import importlib.util,pathlib,time,json
p=pathlib.Path(__file__).resolve().parent;s=importlib.util.spec_from_file_location('mw',p/'meter-watch.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);w=m.MeterWatch()
try:
 end=time.monotonic()+5
 while not w.ready():
  assert time.monotonic()<end;time.sleep(.02)
 before=len(w.rows);time.sleep(.15);w.pump();batch=w.rows[before:];assert batch,'No queued packets received'
 ages=[(r['drained_ns']-r['ns'])/1e6 for r in batch];assert max(ages)>=100,'Kernel timestamp did not retain delayed arrival'
 assert all('kernel_receive_realtime_ns' in r for r in batch)
 report={'passed':True,'RF_commands':[],'deliberate_drain_pause_ms':150,'queued_packets':len(batch),'oldest_queue_age_ms':max(ages),'newest_queue_age_ms':min(ages),'rows':batch};(p/'queue-age-check.json').write_text(json.dumps(report,indent=2)+'\n');print({k:v for k,v in report.items() if k!='rows'})
finally:w.close()
