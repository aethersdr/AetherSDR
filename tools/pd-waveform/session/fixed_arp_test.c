#define main fixed_arp_program_main
#include "pd_fixed_arp.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
int main(void)
{
    const char* bad[]={NULL,"","-1","+3"," 3","3 ","3x","2","1024","99999","3\n"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){assert(lifetime_fd(bad[i])==-1);}
    assert(lifetime_fd("3")==3 && lifetime_fd("1023")==1023);
    assert(!lifetime_ended(POLLIN) && !lifetime_ended(0));
    assert(lifetime_ended(POLLHUP) && lifetime_ended(POLLIN|POLLHUP));
    assert(lifetime_ended(POLLERR) && lifetime_ended(POLLNVAL));
    struct profile p={.mac={2,1,2,3,4,5},.index=5};struct neighbour n={0};assert(!matches(&n,&p));
    n=(struct neighbour){.present=1,.flags=ATF_COM|ATF_PERM,.family=ARPHRD_ETHER,.mac={2,1,2,3,4,5}};assert(matches(&n,&p));
    n.flags=ATF_COM;assert(!matches(&n,&p));n.flags=ATF_COM|ATF_PERM|ATF_PUBL;assert(!matches(&n,&p));n.flags=ATF_COM|ATF_PERM;
    n.mac[5]^=1;assert(!matches(&n,&p));n.mac[5]^=1;n.family=0;assert(!matches(&n,&p));
    assert(restore(-1,0,&p)); /* Preexisting entries require zero restore ioctls. */
    struct arpreq r;request(&r);assert(!strcmp(r.arp_dev,"eth0.10") && r.arp_pa.sa_family==AF_INET);
    struct in_addr a;assert(inet_pton(AF_INET,"172.30.2.1",&a)==1);assert(((struct sockaddr_in*)&r.arp_pa)->sin_addr.s_addr==a.s_addr);
    puts("PASS: only exact self permanent identity matches; foreign/dynamic/proxy identities rejected; unowned state preserved; fixed request target");
}
