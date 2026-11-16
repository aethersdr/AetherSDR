import json,re,sys
from pathlib import Path
p=Path(__file__).resolve().parent;label=sys.argv[1];rows=json.loads((p/'5w'/(label+'-events.json')).read_text())
intent=next(x for x in rows if x['event']=='operator_key_intent');records=[];last_count=0;gaps=[]
for e in rows:
 if e['event']!='service_status':continue
 s=e['result']['body'];record=s.get('diagnostics',{}).get('alignment',{})
 count=record.get('count',0)
 if count==last_count:continue
 if count>last_count+1:gaps.append({'before':last_count,'after':count})
 last_count=count
 text=record['text'];fields=dict(re.findall(r'(\w+)=([^ ]+)',text))
 event={'count':count,'host_elapsed_s':e['elapsed_s'],'record':record,'fields':fields,'observed_before_current_intent':e['monotonic_ns']<intent['monotonic_ns']}
 if fields.get('reason')=='acquisition':
  def delta(a,b):
   first,last=int(fields[a]),int(fields[b]);return (last-first)/1e6 if first>0 and last>=first else None
  event.update(key_to_submission_ms=delta('key_ns','submitted_ns'),queue_ms=delta('submitted_ns','started_ns'),worker_ms=delta('started_ns','completed_ns'),consume_ms=delta('completed_ns','consumed_ns'),key_to_result_ms=delta('key_ns','consumed_ns'),accepted=float(fields['delay'])>=0)
 records.append(event)
tones=[e for e in rows if e['event']=='service_status' and e['result']['body'].get('measurement',{}).get('tones')]
end=next(e for e in rows if e['event']=='measurement_outcome');assert rows[-1]['event']=='idle_session_held' and not end['service']['keyed']
result={'label':label,'events':records,'missed_record_counts':gaps,'first_observed_tones_after_intent_s':tones[0]['elapsed_s']-intent['elapsed_s'] if tones else None,'GUI_outcome':end['widgets']['pdMeasureState']['value'],'diagnostics_at_end':end['service']['diagnostics'],'faults_at_end':end['service']['faults'],'released':True}
(p/(label+'-timing.json')).write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
