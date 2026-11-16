#define PD_START_TEST
#define PD_START_MAIN start_program_main
#include "pd_start_session.c"
#undef PD_START_MAIN
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
static int installation=-1,launches,installation_calls;
static struct pd_iq_selection launched;
int pd_start_test_installation(void){++installation_calls;return installation;}
int pd_start_test_launch(const char* id,uint32_t gui,int control,int lease,struct pd_iq_selection selection)
{
    assert(service_uuid(id) && gui==1 && control==0 && lease==installation && lease>=0);
    assert(fcntl(lease,F_GETFD)>=0);launched=selection;++launches;return 42;
}
static void frame_u32(unsigned char* bytes,uint32_t value)
{
    bytes[0]=(unsigned char)(value>>24);bytes[1]=(unsigned char)(value>>16);
    bytes[2]=(unsigned char)(value>>8);bytes[3]=(unsigned char)value;
}
int main(void)
{
    const char* uuid="0D3EA0E8-B052-4AC6-9D32-D96918634469";
    struct pd_iq_selection selection={1,0};
    assert(start_session_launch(uuid,1,selection)==6 && !launches);
    for(unsigned channel=1;channel<=4;++channel){
        selection=(struct pd_iq_selection){channel,0x40000002};
        installation=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(installation>=0);
        assert(start_session_launch(uuid,1,selection)==42 && launches==(int)channel);
        assert(launched.channel==channel && launched.pan==0x40000002);
        assert(fcntl(installation,F_GETFD)<0 && errno==EBADF);installation=-1;
    }
    int before=installation_calls;
    const struct pd_iq_selection bad_selection[]={{0,1},{5,1},{UINT32_MAX,1},{2,0}};
    for(unsigned i=0;i<sizeof(bad_selection)/sizeof(bad_selection[0]);++i){
        assert(start_session_launch(uuid,1,bad_selection[i])==5 && installation_calls==before);
    }
    unsigned char frame[56]={0};memcpy(frame,"PDSTART1",8);memcpy(frame+8,uuid,36);
    frame_u32(frame+44,0x0321b4de);
    char id[37];uint32_t gui=0;assert(start_frame(frame,48,id,&gui,&selection) && gui==0x0321b4de && selection.channel==1 && !selection.pan);
    assert(!start_frame(frame,56,id,&gui,&selection));
    frame[0]='x';assert(!start_frame(frame,48,id,&gui,&selection));frame[0]='P';
    frame[16]='/';assert(!start_frame(frame,48,id,&gui,&selection));frame[16]='-';
    memset(frame+44,0,4);assert(!start_frame(frame,48,id,&gui,&selection));frame[47]=1;
    frame[7]='2';frame_u32(frame+52,0x40000002);
    for(unsigned channel=1;channel<=4;++channel){
        frame_u32(frame+48,channel);
        assert(start_frame(frame,56,id,&gui,&selection) && gui==1 && selection.channel==channel && selection.pan==0x40000002);
    }
    for(size_t size=0;size<57;++size){if(size!=56){assert(!start_frame(frame,size,id,&gui,&selection));}}
    const uint32_t bad_channels[]={0,5,UINT32_MAX};
    for(unsigned i=0;i<sizeof(bad_channels)/sizeof(bad_channels[0]);++i){frame_u32(frame+48,bad_channels[i]);assert(!start_frame(frame,56,id,&gui,&selection));}
    frame_u32(frame+48,1);frame_u32(frame+52,0);assert(!start_frame(frame,56,id,&gui,&selection));frame_u32(frame+52,0x40000002);
    assert(pd_iq_selection_parse(NULL,NULL,&selection) && selection.channel==1 && !selection.pan);
    const char* bad_channel[]={NULL,"","0","5","01","+1"," 1","1;id","4294967297"};
    for(unsigned i=0;i<sizeof(bad_channel)/sizeof(bad_channel[0]);++i){assert(!pd_iq_selection_parse(bad_channel[i],"0x40000002",&selection));}
    const char* bad_pan[]={NULL,"","0x1","0x4000000g","0x40000002\n","0x400000020","+x40000002"};
    for(unsigned i=0;i<sizeof(bad_pan)/sizeof(bad_pan[0]);++i){assert(!pd_iq_selection_parse("1",bad_pan[i],&selection));}
    assert(!pd_iq_selection_parse("2","0x00000000",&selection));
    assert(pd_iq_selection_parse("4","0xABCDEF12",&selection) && selection.channel==4 && selection.pan==0xabcdef12);
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    int executable=open("/proc/self/exe",O_RDONLY);assert(executable>=0);
    assert(!start_peer(pair[0],executable));close(executable);close(pair[0]);close(pair[1]);
    unsigned char received[56];
    for(unsigned version=1;version<=2;++version){
        frame[7]=(unsigned char)('0'+version);size_t size=version==1?48:56;
        assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));pid_t child=fork();assert(child>=0);
        if(!child){close(pair[0]);for(size_t i=0;i<size;++i){assert(write(pair[1],frame+i,1)==1);}_exit(0);}
        close(pair[1]);assert(start_read(pair[0],received)==(int)size && !memcmp(frame,received,size));close(pair[0]);assert(waitpid(child,NULL,0)==child);
        for(size_t short_size=0;short_size<size;++short_size){
            assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));assert(write(pair[1],frame,short_size)==(ssize_t)short_size);close(pair[1]);assert(!start_read(pair[0],received));close(pair[0]);
        }
    }
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));assert(write(pair[1],"PDSTART9",8)==8);assert(!start_read(pair[0],received));close(pair[0]);close(pair[1]);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));alarm(3);assert(!start_read(pair[0],received));alarm(0);close(pair[0]);close(pair[1]);
    puts("PASS: bounded selected/legacy startup, selection propagation, pre-transaction refusal, fragments, truncation and deadline");
    return 0;
}
