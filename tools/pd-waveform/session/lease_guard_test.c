#define main lease_guard_program_main
#include "pd_lease_guard.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
static const char kReady[]="S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1";
static void line(struct guard_state* s,const char* text,int lease)
{
    parse_status(s,text,lease,"USB");
}
static int64_t fake_now;
static int fake_waits;
static char sent[96];
static size_t sent_used;
static int64_t stalled_time(void){fake_now+=500;return fake_now;}
static int64_t fixed_time(void){return 1;}
static ssize_t blocked_send(int fd,const void* bytes,size_t count,int flags)
{
    (void)fd;(void)bytes;(void)count;(void)flags;errno=EAGAIN;return -1;
}
static ssize_t partial_send(int fd,const void* bytes,size_t count,int flags)
{
    (void)fd;assert(flags==(MSG_NOSIGNAL|MSG_DONTWAIT));
    if(count>4){count=4;}assert(sent_used+count<sizeof(sent));memcpy(sent+sent_used,bytes,count);sent_used+=count;return (ssize_t)count;
}
static int blocked_wait(struct pollfd* events,nfds_t count,int timeout)
{
    assert(count==1 && events[0].events==POLLOUT && timeout>0 && timeout<=500);++fake_waits;return 0;
}
int main(void)
{
    uint32_t identity=0;
    const char* bad_identity[]={"","0","0x","+0x1234","0x100000000","0x1234\n","0xGG","0X1234"};
    for(unsigned i=0;i<sizeof(bad_identity)/sizeof(bad_identity[0]);++i){assert(!hex_identity(bad_identity[i],&identity));}
    assert(hex_identity("0x7B1E8009",&identity) && identity==0x7b1e8009);
    assert(!lifetime_ended(POLLIN) && lifetime_ended(POLLIN|POLLHUP));
    struct write_io stalled={blocked_send,blocked_wait,stalled_time};
    assert(!write_command_io(42,"C1|sub slice all\n",&stalled));assert(fake_now<=2500 && fake_waits>0 && fake_waits<=3);
    struct write_io partial={partial_send,blocked_wait,fixed_time};
    assert(write_command_io(42,"C1|sub slice all\n",&partial));assert(sent_used==strlen("C1|sub slice all\n") && !memcmp(sent,"C1|sub slice all\n",sent_used));
    struct guard_state s={.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};line(&s,kReady,0);assert(s.idle);
    line(&s,"S1|interlock timeout=0 tx_delay=0",0);assert(s.idle);
    line(&s,"S1|interlock band 4 state=TRANSMITTING",0);assert(s.idle);
    const char* bad[]={
      "S0|interlock state=READY",
      "S0|interlock tx_client_handle=0x00000001 state=READY reason= source= tx_allowed=1",
      "S0|interlock tx_client_handle=0x00000000 state=READY reason= source=MIC tx_allowed=1",
      "S0|interlock tx_client_handle=0x00000000 state=READY reason=FAULT source= tx_allowed=1",
      "S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=0",
      "S0|interlock tx_client_handle=0x00000000 state=TRANSMITTING reason= source= tx_allowed=1",
      "S0|interlock tx_client_handle=0x00000000 state=READY state=READY reason= source= tx_allowed=1",
      "S0|interlock tx_client_handle=0 state=READY reason= source= tx_allowed=1"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){line(&s,kReady,0);assert(s.idle);line(&s,bad[i],0);assert(!s.idle);}
    line(&s,kReady,0);line(&s,bad[5],1);assert(s.idle); // old lease statuses are never state evidence
    s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};line(&s,"R1|0|",0);line(&s,"R3|0|0 1 ",0);assert(!snapshot_complete(&s) && !safe_mode(&s));
    line(&s,"S1|slice 0 mode=PD",0);assert(!snapshot_complete(&s));line(&s,"S1|slice 1 mode=LSB",0);assert(snapshot_complete(&s) && !safe_mode(&s) && !other_pd(&s));
    line(&s,"S1|slice 1 mode=PD",0);assert(other_pd(&s));line(&s,"S1|slice 1 in_use=0",0);assert(!other_pd(&s) && snapshot_complete(&s));
    line(&s,"S1|slice 0 mode=USB client_handle=0x1234 pan=0x40000000",0);assert(safe_mode(&s));line(&s,"S1|slice 0 mode=PD mode=USB",0);assert(s.refresh && !s.snapshot);
    s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};line(&s,"R1|0|",0);line(&s,"R3|0|",0);assert(snapshot_complete(&s) && safe_mode(&s));
    s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};line(&s,"R3|0|0 0",0);assert(s.refresh && !s.inventory_known);
    s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};line(&s,"R3|0|8",0);assert(s.refresh && !s.inventory_known);
    s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};line(&s,"R1|500000A3|bad",0);assert(s.query_rejected && !snapshot_complete(&s));
    s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};line(&s,"R900002|0|",1);assert(!s.restore_ack);
    s.restore_sent=1;line(&s,"R900002|0|",1);assert(s.restore_ack);line(&s,"S1|slice 0 mode=USB",0);assert(s.restore_mode_seen);
    s=(struct guard_state){.target=0,.restore_sent=1};line(&s,"R900002|50000001|no",1);assert(s.restore_rejected && !s.restore_ack);
    struct input in={0};s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};
    const char* split="S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1\r\n";
    for(size_t i=0;i<strlen(split);++i){feed(&s,&in,(const unsigned char*)split+i,1,0,"USB");}assert(s.idle && !s.refresh);
    const char* embedded="S0|interlock tx_client_handle=0x00000000 state=REA\rDY reason= source= tx_allowed=1\n";
    feed(&s,&in,(const unsigned char*)embedded,strlen(embedded),0,"USB");assert(!s.idle && s.refresh);
    s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};in=(struct input){0};unsigned char longline[5000];memset(longline,'A',sizeof(longline));longline[sizeof(longline)-1]='\n';
    feed(&s,&in,longline,sizeof(longline),0,"USB");assert(s.refresh && s.protocol_errors==1 && !in.used && !in.discard);
    s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};in=(struct input){0};
    const char inventory_del[]="S5F0DE2AE|waveform installed_list=AetherHealth\177\177v0.1.0,NereusNativeProbe\177\177v0.1.2\n";
    for(size_t i=0;i<sizeof(inventory_del)-1;++i){feed(&s,&in,(const unsigned char*)inventory_del+i,1,0,"USB");}
    assert(!s.refresh && !s.protocol_errors && !s.idle && !s.snapshot);
    const char bad_del[]="S0|interlock state=READY\177\n";
    feed(&s,&in,(const unsigned char*)bad_del,sizeof(bad_del)-1,0,"USB");assert(s.refresh && s.protocol_errors==1 && !s.idle);
    s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};in=(struct input){0};
    const char forged_inventory[]="SZZ|waveform installed_list=A\177B\n";
    feed(&s,&in,(const unsigned char*)forged_inventory,sizeof(forged_inventory)-1,0,"USB");assert(s.refresh && s.protocol_errors==1);
    assert(!known_mode("PD") && !known_mode("USB\nC1|xmit 1") && known_mode("USB") && known_mode("LSB"));
    const char* changes[]={
        "S1|slice 0 client_handle=0x5678", "S1|slice 0 pan=0x40000001",
        "S1|slice 0 client_handle=bad", "S1|slice 0 pan=bad",
        "S1|slice 0 client_handle=0x1234 client_handle=0x1234",
        "S1|slice 0 pan=0x40000000 pan=0x40000000", "S1|slice 0 in_use=bad", "S1|slice 0 in_use=0"};
    for(unsigned i=0;i<sizeof(changes)/sizeof(changes[0]);++i){
        s=(struct guard_state){.target=0,.expected_owner=0x1234,.expected_pan=0x40000000};
        line(&s,kReady,0);line(&s,"R1|0|",0);line(&s,"R3|0|0",0);
        line(&s,"S1|slice 0 mode=PD",0);assert(!restore_admitted(&s));
        line(&s,"S1|slice 0 client_handle=0x1234 pan=0x40000000",0);assert(restore_admitted(&s));
        line(&s,changes[i],0);assert(!restore_admitted(&s));
        if(!i){line(&s,"S1|slice 0 mode=USB",0);assert(!safe_mode(&s));}
        if(i==7){
            line(&s,"S1|slice 0 mode=PD",0);assert(!target_owned(&s) && !restore_admitted(&s));
            line(&s,"S1|slice 0 client_handle=0x5678 pan=0x40000000",0);assert(!restore_admitted(&s));
        }
    }
    puts("PASS: guardian full idle tuple, lease-status isolation, complete inventory, duplicate/malformed fields, other PD slices, restore ACK/state and split/oversized/control-character input");
}
