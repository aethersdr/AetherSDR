from pathlib import Path
import subprocess,shlex
root=Path('/home/jeremy/AetherSDR/build/aetherpd-source');out=Path(__file__).resolve().parent
build=out.parent/'first-install-desktop-monitor/project'
source=out/'binding-fixture.c'
source.write_text(r'''#define main pd_hidden_main
#include "/home/jeremy/AetherSDR/build/aetherpd-source/tools/pd-waveform/pd_waveform.c"
#undef main
#include <assert.h>
int main(int argc,char** argv) {
 assert(argc==2);setenv("AETHER_PD_FEEDBACK","local",1);setenv("AETHER_PD_GUI_HANDLE","0x11223344",1);setenv("AETHER_PD_FEEDBACK_PAN","0x40000001",1);
 for(unsigned channel=1;channel<=4;++channel){
  char ch[2]={(char)('0'+channel),0};setenv("AETHER_PD_DAXIQ",ch,1);
  wf_t w;assert(wf_init(&w));w.api=-1;w.tx_in=0x81000001;w.calibration_control=1;
  for(unsigned phase=0;phase<3;++phase){
   if(phase==1){char line[160];snprintf(line,sizeof(line),"S0|display pan 0x40000001 client_handle=0x11223344 daxiq_channel=%u center=7.150000",channel);api_line(&w,line);}
   if(phase==2){char line[]="S0|display pan 0x40000001 removed";api_line(&w,line);}
   char path[4096],body[4096];int size=status_json(&w,body,sizeof(body));assert(size>0 && size<(int)sizeof(body));
   snprintf(path,sizeof(path),"%s/channel-%u-phase-%u.json",argv[1],channel,phase);FILE* f=fopen(path,"w");assert(f && fwrite(body,1,(size_t)size,f)==(size_t)size && !fclose(f));
  }
  pd_acquire_destroy(w.acquire);pd_core_destroy(w.core);pd_align_destroy(w.align);pd_eq_destroy(w.eq);pd_eq_destroy(w.trusted_eq);free(w.ref.buf);free(w.fbk.buf);free(w.acq_tx);free(w.acq_rx);
 }
 return 0;
}
''')
lines=subprocess.check_output(['ninja','-C',str(build),'-t','commands','pd_calibration_dispatch_test'],text=True).splitlines()
compile=next(shlex.split(line) for line in lines if ' -c ' in line and 'calibration_dispatch_test.c' in line)
if compile[0]=='ccache':compile=compile[1:]
old=compile[compile.index('-o')+1];obj=str(out/'binding-fixture.o');compile[compile.index('-o')+1]=obj;compile[compile.index('-c')+1]=str(source)
for flag in ['-MF','-MT']:
 if flag in compile:
  i=compile.index(flag);del compile[i:i+2]
if '-MD' in compile:compile.remove('-MD')
subprocess.run(compile,cwd=build,check=True)
link=shlex.split(lines[-1])[2:-2];link=[obj if x==old else x for x in link if not x.startswith('-Wl,--dependency-file=')]
link[link.index('-o')+1]=str(out/'binding-fixture');subprocess.run(link,cwd=build,check=True)
(out/'native').mkdir(exist_ok=True);subprocess.run([str(out/'binding-fixture'),str(out/'native')],check=True)
start=out/'start-fixture.c';start.write_text(r'''#define PD_START_MAIN pd_hidden_main
#include "/home/jeremy/AetherSDR/build/aetherpd-source/tools/pd-waveform/session/pd_start_session.c"
#undef PD_START_MAIN
int main(void){unsigned char frame[57];size_t n=fread(frame,1,sizeof(frame),stdin);char id[37];uint32_t gui;struct pd_iq_selection selection;if(!start_frame(frame,n,id,&gui,&selection)){return 2;}printf("{\"gui_client_id\":\"%s\",\"gui_handle\":%u,\"iq_channel\":%u,\"feedback_pan\":%u}\n",id,gui,selection.channel,selection.pan);return 0;}
''')
subprocess.run(['cc','-O1','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',str(start),'-o',str(out/'start-fixture')],check=True)
print('12 native status fixtures and native start-frame parser built; no sockets or radio')
