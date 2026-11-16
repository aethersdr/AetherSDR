"""Recompute stock captures independently and compare the exact candidate pair."""
import hashlib
import json
import math
from pathlib import Path
import subprocess
import numpy as np

p=Path(__file__).resolve().parent
def independent(raw, measured):
    iq=np.frombuffer(raw,dtype='<f4').reshape(4,16384,2).astype(np.float64)
    assert np.isfinite(iq).all()
    samples=iq[:,:,0]+1j*iq[:,:,1]
    n=16384; rate=48000
    weights=np.array([.27105140069342,.43329793923448,.21812299954311,.06592544638803,.01081174209837,.00077658482522,.00001388721735])
    k=np.arange(7); window=((-1.)**k*weights)@np.cos(2*np.pi*k[:,None]*np.arange(n)[None,:]/n)
    power=np.mean(np.abs(np.fft.fftshift(np.fft.fft(samples*window,axis=1),axes=1)/window.sum())**2,axis=0)
    spectrum=10*np.log10(np.maximum(power,1e-20))
    def peak(hz):
        predicted=int(np.floor(hz*n/rate+.5))+n//2
        index=predicted-7+int(np.argmax(power[predicted-7:predicted+8]))
        a,b,c=np.polyfit([-1.,0.,1.],spectrum[index-1:index+2],2)
        offset=-b/(2*a) if abs(a)>5e-13 else 0
        if abs(offset)>1:offset=0
        return (index-n//2+offset)*rate/n,float(a*offset**2+b*offset+c)
    tones=[peak(f) for f in (710,1900)]
    tone_db=10*math.log10(sum(10**(v/10) for _,v in tones)/2)
    pep_db=20*math.log10(sum(10**(v/20) for _,v in tones))
    errors=[abs(pep_db-measured['fundamental_pep_db_raw_iq'])]
    for product in measured['products']:
        spacing=tones[1][0]-tones[0][0]
        hz=tones[1][0]+(product['order']-1)*spacing/2 if product['upper'] else tones[0][0]-(product['order']-1)*spacing/2
        _,level=peak(hz)
        errors.append(abs(level-tone_db-product['dbc_per_tone']))
    assert max(errors)<1e-8,errors
    return {'max_difference_db':max(errors),'iq_sha256':hashlib.sha256(raw).hexdigest()}

results={}
for case in ('stock-off-01','stock-off-02','stock-off-03','stock-on-03'):
    root=p/case; r=json.loads((root/'result.json').read_text())
    assert r['completed'] and r['idle_verified'] and not r['cleanup_errors'] and len(r['key_intents'])==1
    assert len(r['captures'])==4 and r['guard_final']['state']=='ended'
    raw=(root/'four-frames.f32le').read_bytes()
    measured=json.loads(subprocess.check_output([str(p/'analyze-raw'),str(root/'four-frames.f32le')],text=True))
    for i,frame in enumerate(r['captures']):
        assert hashlib.sha256(raw[i*131072:(i+1)*131072]).hexdigest()==frame['sha256']
    results[case]={'analysis':measured,'independent_recheck':independent(raw,measured),'source':r['source'],
                   'max_radio_sampled_forward_watts':max(x['meters']['fwdPowerInstant'] for x in r['rows']),
                   'GUI_stop_s':r['GUI_stop_s'],'equalizer_states_at_capture_completion':[f['stock_apd'].get('equalizer_active') for f in r['captures']],
                   'APD_states_at_capture_completion':[f['stock_apd'].get('state') for f in r['captures']]}
on=results['stock-on-03']['analysis'];off=results['stock-off-03']['analysis']
assert results['stock-on-03']['source']==results['stock-off-03']['source']
assert results['stock-on-03']['equalizer_states_at_capture_completion']==['1']*4
aether=json.loads((p.parent/'candidate-rf/10w/comparison.json').read_text())
pair_delta=on['fundamental_pep_db_raw_iq']-off['fundamental_pep_db_raw_iq']
comparison_delta=on['fundamental_pep_db_raw_iq']-aether['fundamental_pep_db_raw_iq']
report={'stock_cases':results,'stock_on_minus_off_power_db':pair_delta,'stock_on_minus_aether_on_power_db':comparison_delta,
        'stock_pair_power_match_within_0_5db':abs(pair_delta)<=.5,'corrected_comparison_power_match_within_0_5db':abs(comparison_delta)<=.5,
        'stock_improvement_db':[], 'aether_vs_stock_db':[], 'ADC_headroom_known':False, 'absolute_power_calibrated':False}
for stock,base,apd in zip(on['products'],off['products'],aether['products']):
    assert (stock['order'],stock['upper'])==(base['order'],base['upper'])==(apd['order'],apd['upper'])
    report['stock_improvement_db'].append({'order':stock['order'],'upper':stock['upper'],'improvement_db':base['dbc_per_tone']-stock['dbc_per_tone']})
    report['aether_vs_stock_db'].append({'order':stock['order'],'upper':stock['upper'],'aether_dbc_per_tone':apd['dbc_per_tone'],'stock_dbc_per_tone':stock['dbc_per_tone'],'positive_means_aether_lower_db':stock['dbc_per_tone']-apd['dbc_per_tone']})
(p/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='stock_cases'}))
