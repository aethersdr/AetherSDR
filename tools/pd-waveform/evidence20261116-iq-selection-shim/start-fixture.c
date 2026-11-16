#define PD_START_MAIN pd_hidden_main
#include "/home/jeremy/AetherSDR/build/aetherpd-source/tools/pd-waveform/session/pd_start_session.c"
#undef PD_START_MAIN
int main(void){unsigned char frame[57];size_t n=fread(frame,1,sizeof(frame),stdin);char id[37];uint32_t gui;struct pd_iq_selection selection;if(!start_frame(frame,n,id,&gui,&selection)){return 2;}printf("{\"gui_client_id\":\"%s\",\"gui_handle\":%u,\"iq_channel\":%u,\"feedback_pan\":%u}\n",id,gui,selection.channel,selection.pan);return 0;}
