"""One explicitly invoked 5 W GUI measurement on desktopb1256a165; no automatic retries or rekey."""
import importlib.util, json, os, subprocess, sys, time
from pathlib import Path

ROOT=Path(__file__).resolve().parent
W=Path('/home/jeremy/AetherSDR/build/aetherpd-source')
label=sys.argv[1]
assert label in ('off-01','on-01','off-02','on-02')
output=ROOT/(label+'-events.json')
assert not output.exists(),'Do not repeat a named RF attempt'
instance=json.loads((ROOT/'instance.json').read_text())
env=dict(v.split(b'=',1) for v in Path('/proc',str(instance['pid']),'environ').read_bytes().split(b'\0') if b'=' in v)
assert b'AETHER_AUTOMATION_NO_TX' not in env and env.get(b'AETHER_AUTOMATION_ALLOW_TX')==b'1'
spec=importlib.util.spec_from_file_location('probe',W/'tools/automation_probe.py')
probe=importlib.util.module_from_spec(spec);spec.loader.exec_module(probe)
start=time.monotonic();events=[]
def record(event,**values):
    events.append({'elapsed_s':round(time.monotonic()-start,3),'monotonic_ns':time.monotonic_ns(),'event':event,**values})
    output.write_text(json.dumps(events,indent=2)+'\n')
def request(req,save=True):
    b=probe.Bridge(instance['socket'])
    try: result=b.request(req,5)
    finally:b.close()
    if save:record('bridge',request=req,result=result)
    assert result.get('ok'),result
    return result
def click(target):return request({'cmd':'invoke','target':target,'action':'click'})
def widgets():
    result={}
    def visit(x):
        if isinstance(x,dict):
            name=x.get('objectName','')
            if name.startswith('pdMeasure') or name=='daxPanel':
                result[name]={k:v for k,v in x.items() if k in ('value','enabled','visible')}
            for key in ('roots','children'):
                for child in x.get(key,[]):visit(child)
        elif isinstance(x,list):
            for child in x:visit(child)
    visit(request({'cmd':'dumpTree','target':'AetherPDMeasurementDialog'},False))
    return result
def wait_enabled(name,seconds):
    deadline=time.monotonic()+seconds
    while True:
        state=widgets()
        if state.get(name,{}).get('enabled'):return state
        if time.monotonic()>=deadline:
            record('not_ready',target=name,widgets=state);raise RuntimeError(name+' not enabled')
        time.sleep(.1)
def status():
    result=subprocess.run([str(ROOT.parents[1]/'first-iq-live/vault-v3'),'aetherpd/status'],
                          capture_output=True,text=True,timeout=15)
    body=json.loads(result.stdout);record('service_status',result=body)
    assert body['http_status']==200,body
    return body['body']

for prior_path in ROOT.parent.glob('*w/*-events.json'):
    prior=json.loads(prior_path.read_text())
    ends=[e for e in prior if e['event']=='idle_session_held']
    if ends:
        assert time.monotonic_ns()-ends[-1]['monotonic_ns']>=31_000_000_000,'Preserve the30second cooldown'
radio=request({'cmd':'get','model':'radio'})['radio']
assert radio['connected'] and radio['serial']=='2125-1213-8600-8895' and not radio['transmitting'] and not radio['fullDuplex']
power=request({'cmd':'get','model':'transmit'})['transmit']
assert power['rfPower']==5 and power['maxPowerLevel']<=10 and not power['apdEnabled']
slices=request({'cmd':'get','model':'slices'})['slices']
assert len(slices)==1 and slices[0]['txSlice'] and slices[0]['frequency']==7.15
assert slices[0]['txAntenna']=='ANT1' and slices[0]['rxAntenna']=='RX_A'
pans=request({'cmd':'get','model':'pans'})['pans']
assert len(pans)==1 and pans[0]['centerMhz']==7.15 and pans[0]['rfGain']==0
if label.startswith('on-'):
    previous=json.loads((ROOT/(label.replace('on-','off-')+'-events.json')).read_text())
    assert previous[-1]['event']=='idle_session_held'
    assert time.time()-previous[-1]['wall_time']>=31
body=status();assert body['available'] and body['registered'] and not body['keyed']
assert body['correction_enabled']==(label.startswith('on-'))
assert body['feedback_binding']['valid'] and body['feedback_binding']['channel']==4 and body['feedback_binding']['gui_handle']==int(pans[0]['clientHandle'],16)
if slices[0]['mode']!='PD':request({'cmd':'slice','action':'mode','value':'PD','id':str(slices[0]['sliceId'])})
request({'cmd':'invoke','target':'pdMeasureDummyLoad','action':'setChecked','value':'true'})
request({'cmd':'invoke','target':'pdMeasurePowerLimit','action':'setValue','value':'15'})
wait_enabled('pdMeasureArm',6);click('pdMeasureArm')
deadline=time.monotonic()+10
while True:
    body=status()
    if body.get('calibration_state')=='armed' and body.get('source_purpose')=='measurement':break
    assert time.monotonic()<deadline,'Source arm not observed';time.sleep(.2)
assert body['measurement']['source_id']!='0000000000000000' and not body['keyed']
wait_enabled('pdMeasurePrepare',4);click('pdMeasurePrepare')
state=wait_enabled('pdMeasureTransmit',6);record('receive_ready',widgets=state)
power=request({'cmd':'get','model':'transmit'})['transmit']
assert power['rfPower']==5 and power['maxPowerLevel']<=10 and not power['apdEnabled']
record('operator_key_intent',frequency_mhz=7.15,nominal_watts=5,measured_abort_watts=15,
       source_limit_seconds=8,feedback_channel=4,independent_native_cleanup_interval_seconds=9.0,desktop_authorization_seconds=10.0,one_positive_GUI_action=True)
key_request=time.monotonic();saw_key=False
try:
    click('pdMeasureTransmit')
    while time.monotonic()-key_request<12:
        radio=request({'cmd':'get','model':'radio'})['radio']
        request({'cmd':'get','model':'meters'})
        assert radio['connected'] and not radio['fullDuplex']
        saw_key |= radio['transmitting']
        if radio['txPower'] is not None and radio['txPower']>15:
            click('pdMeasureStop');raise RuntimeError('Measured forward power exceeded 15 W')
        if saw_key and not radio['transmitting']:break
        time.sleep(.1)
finally:
    radio=request({'cmd':'get','model':'radio'})['radio']
    if radio['transmitting']:
        click('pdMeasureStop')
        request({'cmd':'key','action':'ptt','value':'off'})
    deadline=time.monotonic()+12
    while True:
        state=widgets()
        if not state.get('pdMeasureStop',{}).get('enabled'):break
        if time.monotonic()>=deadline:
            record('release_not_confirmed',widgets=state);raise RuntimeError('Measurement release not confirmed')
        time.sleep(.15)
    radio=request({'cmd':'get','model':'radio'})['radio']
    assert not radio['transmitting']
    body=status();assert not body['keyed']
    record('measurement_outcome',saw_key=saw_key,widgets=state,service=body)
    if state.get('pdMeasureCancelSource',{}).get('enabled'):click('pdMeasureCancelSource')
    record('idle_session_held',wall_time=time.time(),no_automatic_rearm=True)
print(json.dumps({'case':label,'saw_key':saw_key,'result':state.get('pdMeasureResult'),'state':state.get('pdMeasureState')}))
