#define main pd_hidden_main
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
