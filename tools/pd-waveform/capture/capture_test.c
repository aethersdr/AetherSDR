#define main capture_constructor_main
#include "pd_capture_bootstrap.c"
#undef main
#undef NDEBUG
#include <assert.h>

/* Interpret only the classic BPF operations emitted by this constructor. */
static unsigned filter_packet(const unsigned char* packet,unsigned length)
{
    struct sock_filter code[48];
    unsigned count=capture_filter(code,44000,0x12345678,48000),a=0,x=0;
    for(unsigned pc=0;pc<count;++pc){
        struct sock_filter c=code[pc];
        if(BPF_CLASS(c.code)==BPF_RET){return c.k;}
        if(BPF_CLASS(c.code)==BPF_LD){
            if(BPF_MODE(c.code)==BPF_LEN){a=length;continue;}
            unsigned offset=c.k+(BPF_MODE(c.code)==BPF_IND?x:0);
            unsigned size=BPF_SIZE(c.code)==BPF_W?4:BPF_SIZE(c.code)==BPF_H?2:1;
            if(offset>length || size>length-offset){return 0;}
            a=0;for(unsigned j=0;j<size;++j){a=(a<<8)|packet[offset+j];}
        }else if(BPF_CLASS(c.code)==BPF_LDX){
            assert(c.code==(BPF_LDX|BPF_B|BPF_MSH));
            if(c.k>=length){return 0;}x=(packet[c.k]&15)*4;
        }else if(BPF_CLASS(c.code)==BPF_ALU){
            assert(BPF_OP(c.code)==BPF_AND);a&=c.k;
        }else if(BPF_CLASS(c.code)==BPF_JMP){
            int yes;
            if(BPF_OP(c.code)==BPF_JEQ){yes=a==c.k;}
            else if(BPF_OP(c.code)==BPF_JGE){yes=a>=c.k;}
            else{assert(BPF_OP(c.code)==BPF_JGT);yes=a>c.k;}
            pc+=yes?c.jt:c.jf;
        }else{assert(0);}
    }
    assert(0);return 0;
}

int main(void)
{
    unsigned char packet[56]={0};
    packet[0]=0x45;packet[9]=17;
    pd_iq_put32(packet+12,0xc0a83264);pd_iq_put32(packet+16,0xac1e0201);
    packet[20]=50001>>8;packet[21]=50001&255;
    packet[22]=44000>>8;packet[23]=44000&255;
    packet[25]=36;
    pd_iq_put32(packet+28,0x18600000);pd_iq_put32(packet+32,0x12345678);
    pd_iq_put32(packet+36,0x1c2d);pd_iq_put32(packet+40,0x534c02e4);
    assert(filter_packet(packet,sizeof(packet)));
    const unsigned offsets[]={0,9,12,15,16,19,20,21,22,23,32,35,36,39,40,43};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i){
        packet[offsets[i]]^=0x80;
        assert(!filter_packet(packet,sizeof(packet)));
        packet[offsets[i]]^=0x80;
    }
    packet[6]=0x20;assert(!filter_packet(packet,sizeof(packet)));packet[6]=0;
    for(unsigned length=0;length<56;++length){assert(!filter_packet(packet,length));}
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_DGRAM,0,pair));
    assert(capture_private_output(pair[0]));assert(capture_private_output(pair[1]));
    unsigned port=0;assert(!capture_endpoint(pair[0],&port));
    close(pair[0]);close(pair[1]);
    uint64_t value=0;
    assert(!capture_number("-1",10,6,&value));assert(!capture_number(" 3",10,6,&value));
    assert(!capture_number("12junk",16,8,&value));
    assert(capture_number("12345678",16,8,&value) && value==0x12345678);
    puts("capture constructor boundary/filter tests passed");return 0;
}
