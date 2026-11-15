#define main iq_stream_program_main
#include "pd_iq_stream.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
static const char kFull[]="S12345678|stream 0x20000001 type=dax_iq daxiq_channel=1 daxiq_rate=48000 client_handle=0x12345678 client_gui_handle=0x11223344 ip=172.30.2.1 active=1\n";
static struct stream_state state(void)
{
    return (struct stream_state){.client=0x12345678,.gui=0x11223344,.stream=0x20000001,.issued=127};
}
static int wire(struct stream_state* s,const char* text)
{
    struct stream_input in={0};return feed(s,&in,(const unsigned char*)text,strlen(text));
}
static void reject(const char* text)
{
    struct stream_state s=state();assert(!wire(&s,text));
}
int main(void)
{
    struct stat stdio[3]={{.st_mode=S_IFCHR|0600,.st_rdev=makedev(1,3)},{.st_mode=S_IFIFO|0600},{.st_mode=S_IFIFO|0600}};
    int access[3]={O_RDONLY,O_WRONLY,O_WRONLY};assert(stream_standard_policy(stdio,access));
    access[0]=O_RDWR;assert(stream_standard_policy(stdio,access));
    access[1]=O_RDONLY;assert(!stream_standard_policy(stdio,access));access[1]=O_WRONLY;
    stdio[0].st_mode=S_IFSOCK|0600;assert(!stream_standard_policy(stdio,access));stdio[0].st_mode=S_IFCHR|0600;
    stdio[2].st_mode=S_IFSOCK|0600;assert(!stream_standard_policy(stdio,access));
    uint32_t n=0;assert(number("ffffffff",16,8,&n) && n==UINT32_MAX);
    assert(descriptor("3")==3 && descriptor("1023")==1023);
    const char* bad[]={NULL,"","-3","+3"," 3","3 ","1024","2","00003","3;id"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){assert(descriptor(bad[i])==-1);}
    assert(!number("100000000",16,9,&n) && !number("4294967296",10,10,&n));
    assert(client_id("0D3EA0E8-B052-4AC6-9D32-D96918634469"));
    assert(!client_id("0D3EA0E8-B052-4AC6-9D32-D96918634469\n"));
    assert(!client_id("0D3EA0E8 B052-4AC6-9D32-D96918634469"));
    struct stream_state s=state();assert(wire(&s,kFull) && s.fields==63);
    char broadcast[sizeof(kFull)];memcpy(broadcast,kFull,sizeof(kFull));memcpy(broadcast+1,"87654321",8);
    s=state();assert(wire(&s,broadcast) && s.fields==63);
    s=state();assert(wire(&s,"S87654321|radio daxiq_available=14 full_duplex_enabled=0\n") && !s.fields);
    /* Framing must not depend on recv() boundaries. */
    for(size_t split=0;split<=strlen(kFull);++split){
        s=state();struct stream_input in={0};assert(feed(&s,&in,(const unsigned char*)kFull,split));
        assert(feed(&s,&in,(const unsigned char*)kFull+split,strlen(kFull)-split) && s.fields==63);
    }
    const char* bad_status[]={
        "S12345678|stream 0x20000001 type=dax_audio\n",
        "S12345678|stream 0x20000001 daxiq_channel=2\n",
        "S12345678|stream 0x20000001 daxiq_rate=96000\n",
        "S12345678|stream 0x20000001 client_handle=0x12345679\n",
        "S12345678|stream 0x20000001 client_gui_handle=0x11223345\n",
        "S12345678|stream 0x20000001 ip=127.0.0.1\n",
        "S12345678|stream 0x20000001 daxiq_channel=1 daxiq_channel=1\n",
        "S12345678|stream 0x20000001 client_handle=0x12345678 client_handle=0x12345678\n",
        "Sxyz|stream 0x20000001 type=dax_iq\n",
        "R970104|0|20000001\n", /* A second create ACK cannot replace ownership. */
        "R970101|0|\n","R970101|0|0x11223345\n","R970108|0|\n","R970105|50000001|\n","R970105|0|unexpected\n",
        "R970105|0|\nR970105|0|\n","R970105|-1|\n","H12345678\n",
        "S12345678|stream 0x20000001 ip=172.30.2.1\rX\n"
    };
    for(unsigned i=0;i<sizeof(bad_status)/sizeof(bad_status[0]);++i){reject(bad_status[i]);}
    s=state();assert(wire(&s,"R970101|0|0x11223344\n") && (s.replies&1u));
    s=state();assert(wire(&s,"R970105|0|DAXIQ rate set succesfully\n") && (s.replies&16u));
    s=state();s.issued=0;assert(!wire(&s,"R970105|0|\n"));
    s=state();s.stream=0;assert(wire(&s,kFull) && s.fields==0 && !s.stream);
    s=state();assert(wire(&s,"S12345678|stream 0x20000002 type=dax_audio daxiq_channel=2\n") && !s.fields);
    s=state();s.issued=15;assert(wire(&s,"S12345678|stream 0x20000001 daxiq_rate=24000\n") && !s.fields);
    s=state();assert(wire(&s,"S12345678|stream 0x20000001 removed\n") && s.removed);
    s=state();s.stream=0;assert(wire(&s,"R970104|0|0x20000001\n") && s.stream==0x20000001);
    s=state();s.client=0;s.stream=0;assert(wire(&s,"V1.4.0.0\r\nH12345678\r\n") && s.client==0x12345678);
    unsigned char zero[]={ 'S','0','|',0,'\n' };struct stream_input in={0};s=state();assert(!feed(&s,&in,zero,sizeof(zero)));
    unsigned char long_line[kLine+2];memset(long_line,'X',sizeof(long_line));s=state();in=(struct stream_input){0};assert(!feed(&s,&in,long_line,sizeof(long_line)));
    assert(!prepared(3,3,3,&n));
    puts("PASS: IQ stream ownership, GUI/client/channel/rate/destination, ACK provenance, fragment boundaries and hostile framing; no sockets, privileges or radio peer");
    return 0;
}
