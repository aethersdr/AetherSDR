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


import base64
results={}
for label in ('off-02','on-02'):
 export=json.loads((p/'5w'/(label+'.json')).read_text())
 assert export['measurement']['valid'] and len(export['captures'])==4
 raw=b''.join(base64.b64decode(frame['iq_f32le'],validate=True) for frame in export['captures'])
 for i,frame in enumerate(export['captures']):
  assert hashlib.sha256(raw[i*131072:(i+1)*131072]).hexdigest()==frame['sha256']
 path=p/'5w'/(label+'.f32le');path.write_bytes(raw)
 measured=json.loads(subprocess.check_output([str(p.parent/'stock-candidate/analyze-raw'),str(path)],text=True))
 check=independent(raw,measured)
 errors=[abs(a['dbc_per_tone']-b['dbc_per_tone']) for a,b in zip(measured['products'],export['measurement']['products'])]
 assert max(errors)<1e-4
 results[label]={'analysis':measured,'independent':check,'gui_max_difference_db':max(errors)}
a=results['on-02']['analysis'];b=results['off-02']['analysis']
report={'cases':results,'power_match_db':a['fundamental_pep_db_raw_iq']-b['fundamental_pep_db_raw_iq'],'improvement_db':[{'order':x['order'],'upper':x['upper'],'db':y['dbc_per_tone']-x['dbc_per_tone']} for x,y in zip(a['products'],b['products'])]}
(p/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='cases'}))
