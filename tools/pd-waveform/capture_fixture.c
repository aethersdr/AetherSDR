/* Explicit local C/Go protocol integration fixture. Inherited socket only;
 * no radio, RF, privileged operation or installed package includes this. */
#include "pd_capture.h"
#include "pd_status.h"
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static int64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) { return 0; }
    return (int64_t)t.tv_sec*1000000000+t.tv_nsec;
}
int main(void)
{
    if (pd_status_open("3")!=3) { return 1; }
    struct pd_capture capture={0};
    struct pd_capture_meta meta={7,7150000,0x40000001,192000,1,1};
    for (unsigned requests=0;requests<64;++requests) {
        struct pollfd fd={.fd=3,.events=POLLIN};
        if (poll(&fd,1,2000)!=1) { return 2; }
        struct pd_status_query q;
        if (pd_status_query(3,&q)!=1) { return 3; }
        if (q.operation=='B') { pd_capture_begin(&capture,q.capture_id,&meta,now_ns()); }
        unsigned char reply[PD_CAPTURE_REPLY];
        size_t size=pd_capture_read(&capture,q.capture_id,q.capture_offset,now_ns(),reply,sizeof(reply));
        if (!pd_status_reply(3,q.nonce,(const char*)reply,size)) { return 4; }
        if (q.operation=='B') {
            unsigned char samples[4096];
            const unsigned char sample[8]={0,0,0,0x3f,0,0,0x80,0xbe};
            for (unsigned i=0;i<sizeof(samples);i+=8) { memcpy(samples+i,sample,8); }
            for (unsigned i=0;i<32;++i) { pd_capture_feed(&capture,&meta,samples,sizeof(samples),now_ns()); }
        }
        if (q.operation=='R' && q.capture_offset==PD_CAPTURE_SAMPLES-PD_CAPTURE_CHUNK) { return 0; }
    }
    return 5;
}
