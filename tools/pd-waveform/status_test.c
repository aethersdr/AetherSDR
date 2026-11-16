#define _GNU_SOURCE
#undef NDEBUG
#include "pd_status.h"
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(void)
{
    assert(pd_status_open(NULL)==-1 && pd_status_open("bad")==-2 && pd_status_open("2")==-2);
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_DGRAM,0,pair));
    char number[16];snprintf(number,sizeof(number),"%d",pair[0]);assert(pd_status_open(number)==pair[0]);
    assert(fcntl(pair[0],F_GETFD)&FD_CLOEXEC);assert(fcntl(pair[0],F_GETFL)&O_NONBLOCK);
    unsigned char nonce[8],request[9]={'?',1,2,3,4,5,6,7,8};
    assert(!pd_status_request(pair[0],nonce));
    assert(send(pair[1],"stop",4,0)==4 && pd_status_request(pair[0],nonce)==-1);
    assert(send(pair[1],request,9,0)==9 && pd_status_request(pair[0],nonce)==1);
    assert(!memcmp(nonce,request+1,8));
    assert(pd_status_reply(pair[0],nonce,"{}",2));unsigned char reply[32];
    assert(recv(pair[1],reply,sizeof(reply),0)==10 && !memcmp(reply,nonce,8) && !memcmp(reply+8,"{}",2));
    assert(!pd_status_reply(pair[0],nonce,"x",16385));
    assert(send(pair[1],request,8,0)==8 && pd_status_request(pair[0],nonce)==-1);
    unsigned char large[100]={0};assert(send(pair[1],large,sizeof(large),0)==sizeof(large));
    assert(pd_status_request(pair[0],nonce)==-1);
    struct pd_status_query query;
    unsigned char capture[21]={'B',1,2,3,4,5,6,7,8,0,0,0,0,0,0,0,9,0,0,4,0};
    assert(send(pair[1],capture,17,0)==17 && pd_status_query(pair[0],&query)==1);
    assert(query.operation=='B' && query.capture_id==9 && !query.capture_offset);
    capture[0]='R';
    assert(send(pair[1],capture,21,0)==21 && pd_status_query(pair[0],&query)==1);
    assert(query.operation=='R' && query.capture_id==9 && query.capture_offset==1024);
    capture[0]='G';capture[19]=0;capture[20]=4;
    assert(send(pair[1],capture,21,0)==21 && pd_status_query(pair[0],&query)==1);
    assert(query.operation=='G' && query.capture_id==9 && query.capture_offset==4);
    capture[20]=0;
    assert(send(pair[1],capture,21,0)==21 && pd_status_query(pair[0],&query)==-1);
    capture[20]=5;
    assert(send(pair[1],capture,21,0)==21 && pd_status_query(pair[0],&query)==-1);
    capture[0]='H';capture[19]=0xc0;capture[20]=0;
    assert(send(pair[1],capture,21,0)==21 && pd_status_query(pair[0],&query)==1);
    assert(query.operation=='H' && query.capture_offset==49152);
    capture[20]=1;
    assert(send(pair[1],capture,21,0)==21 && pd_status_query(pair[0],&query)==-1);
    capture[18]=1;capture[19]=0;capture[20]=0;
    assert(send(pair[1],capture,21,0)==21 && pd_status_query(pair[0],&query)==-1);
    capture[18]=0;
    for (unsigned length=0;length<21;++length) {
        assert(send(pair[1],capture,length,0)==(ssize_t)length && pd_status_query(pair[0],&query)==-1);
    }
    memcpy(large,capture,sizeof(capture));
    assert(send(pair[1],large,sizeof(large),0)==sizeof(large) && pd_status_query(pair[0],&query)==-1);
    capture[16]=0;
    assert(send(pair[1],capture,21,0)==21 && pd_status_query(pair[0],&query)==-1);
    close(pair[0]);close(pair[1]);
    int unconnected=socket(AF_UNIX,SOCK_DGRAM,0);assert(unconnected>=0);
    snprintf(number,sizeof(number),"%d",unconnected);assert(pd_status_open(number)==-2);
    struct sockaddr_un address={.sun_family=AF_UNIX};
    int length=snprintf(address.sun_path+1,sizeof(address.sun_path)-1,"pd-status-test-%ld",(long)getpid());
    assert(length>0 && (size_t)length<sizeof(address.sun_path)-1);
    assert(!bind(unconnected,(struct sockaddr*)&address,(socklen_t)(offsetof(struct sockaddr_un,sun_path)+1+length)));
    assert(pd_status_open(number)==-2);close(unconnected);
    int internet=socket(AF_INET,SOCK_DGRAM,0);assert(internet>=0);
    snprintf(number,sizeof(number),"%d",internet);assert(pd_status_open(number)==-2);close(internet);
    puts("private status descriptor/protocol tests passed");return 0;
}
