#include "pd_owner.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void)
{
    assert(pd_owner_open(NULL)==-1 && pd_owner_open("")==-1);
    const char* bad[]={"0","2","-1","+3","3x","1024","999999999999999999999"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) { assert(pd_owner_open(bad[i])==-2); }
    int p[2];assert(!pipe(p));char text[32];snprintf(text,sizeof(text),"%d",p[1]);assert(pd_owner_open(text)==-2);
    snprintf(text,sizeof(text),"%d",p[0]);assert(pd_owner_open(text)==p[0]);assert(!pd_owner_closed(p[0]));
    int flags=fcntl(p[1],F_GETFL);assert(!fcntl(p[1],F_SETFL,flags|O_NONBLOCK));char data[4096];memset(data,1,sizeof(data));
    while (write(p[1],data,sizeof(data))>0) {}
    assert(!pd_owner_closed(p[0]));close(p[1]);assert(pd_owner_closed(p[0]));close(p[0]);assert(pd_owner_closed(p[0]));
    int s[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,s));snprintf(text,sizeof(text),"%d",s[0]);assert(pd_owner_open(text)==-2);close(s[0]);close(s[1]);
    puts("PASS: lifetime pipe validates direction/type/range and detects EOF even with queued data, without consuming it");return 0;
}
