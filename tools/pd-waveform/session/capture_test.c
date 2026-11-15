#define main capture_program_main
#include "pd_capture_bootstrap.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

// Execute only the instructions emitted by capture_filter; no socket, ioctl,
// capability or radio operation is used by this policy test.
static uint32_t filter_run(const struct sock_filter* code,unsigned count,const unsigned char* packet,size_t bytes)
{
    uint32_t a=0,x=0;
    for(unsigned pc=0;pc<count;++pc){
        struct sock_filter i=code[pc];
        if(i.code==(BPF_LD|BPF_W|BPF_LEN)){a=(uint32_t)bytes;}
        else if(i.code==(BPF_LDX|BPF_B|BPF_MSH)){
            if(i.k>=bytes){return 0;}x=(packet[i.k]&15)*4u;
        }else if(BPF_CLASS(i.code)==BPF_LD){
            size_t at=i.k+(BPF_MODE(i.code)==BPF_IND?x:0);
            unsigned width=BPF_SIZE(i.code)==BPF_W?4:BPF_SIZE(i.code)==BPF_H?2:1;
            if(at>bytes || width>bytes-at){return 0;}a=0;
            for(unsigned n=0;n<width;++n){a=(a<<8)|packet[at+n];}
        }else if(i.code==(BPF_ALU|BPF_AND|BPF_K)){a&=i.k;}
        else if(BPF_CLASS(i.code)==BPF_JMP){
            int yes=0;
            switch(BPF_OP(i.code)){
                case BPF_JEQ:yes=a==i.k;break;
                case BPF_JGE:yes=a>=i.k;break;
                case BPF_JGT:yes=a>i.k;break;
                default:assert(0);
            }
            pc+=yes?i.jt:i.jf;
        }else if(i.code==(BPF_RET|BPF_K)){return i.k;}
        else{assert(0);}
    }
    return 0;
}
static void packet_fixture(unsigned char p[64],unsigned rate)
{
    memset(p,0,64);p[0]=0x45;p[3]=64;p[9]=17;
    pd_iq_put32(p+12,0xc0a83264);pd_iq_put32(p+16,0xac1e0201);
    p[20]=50001>>8;p[21]=50001&255;p[22]=48998>>8;p[23]=48998&255;p[25]=44;
    pd_iq_put32(p+28,0x18600009);pd_iq_put32(p+32,0x20000001);pd_iq_put32(p+36,0x1c2d);
    pd_iq_put32(p+40,0x534c0000|(rate==24000?0x02e3:rate==48000?0x02e4:rate==96000?0x02e5:0x02e6));
}
int main(void)
{
    uint64_t value=0;
    assert(capture_number("20000001",16,8,&value) && value==0x20000001);
    const char* bad[]={"","-1","+3"," 3","3 ","0x10","1;id","18446744073709551616"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){assert(!capture_number(bad[i],10,20,&value));}
    assert(!capture_number("100000000",16,8,&value));
    unsigned rates[]={24000,48000,96000,192000};
    for(unsigned r=0;r<sizeof(rates)/sizeof(rates[0]);++r){
        struct sock_filter code[48];unsigned count=capture_filter(code,48998,0x20000001,rates[r]);assert(count<=48);
        unsigned char p[64];packet_fixture(p,rates[r]);assert(filter_run(code,count,p,sizeof(p))==65535);
        for(size_t n=0;n<56;++n){assert(!filter_run(code,count,p,n));}
        unsigned offsets[]={9,12,13,14,15,16,17,18,19,20,21,22,23,32,33,34,35,36,37,38,39,40,41,42,43};
        for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i){packet_fixture(p,rates[r]);p[offsets[i]]^=1;assert(!filter_run(code,count,p,sizeof(p)));}
        packet_fixture(p,rates[r]);p[0]=0x65;assert(!filter_run(code,count,p,sizeof(p)));
        packet_fixture(p,rates[r]);p[0]=0x44;assert(!filter_run(code,count,p,sizeof(p)));
        packet_fixture(p,rates[r]);p[6]=0x20;assert(!filter_run(code,count,p,sizeof(p)));
        packet_fixture(p,rates[r]);p[7]=1;assert(!filter_run(code,count,p,sizeof(p)));
        packet_fixture(p,rates[r]);p[25]=35;assert(!filter_run(code,count,p,sizeof(p)));
        packet_fixture(p,rates[r]);p[28]^=0x80;assert(!filter_run(code,count,p,sizeof(p)));
        packet_fixture(p,rates[r]);p[29]^=0x0f;assert(filter_run(code,count,p,sizeof(p))==65535);
        assert(!filter_run(code,count,p,PD_IQ_MAX_PACKET+69));
    }
    puts("PASS: fixed capture filter rates, complete flow identity, truncation, fragments and strict numeric arguments; no sockets or privileges");
    return 0;
}
