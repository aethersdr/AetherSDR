#ifndef AETHER_PD_UPLOAD_H
#define AETHER_PD_UPLOAD_H
/* Receives envelope, plan, candidate, and optional previous release. The caller
 * has authenticated the private request configuration. Returned descriptors are
 * read-only unlinked tmpfs files; no paths or commands are accepted on the wire.
 * Success means transfer verified, not signed release admission or installation.
 * The caller closes every returned descriptor and retains the root handoff gate. */
int pd_upload_receive(int channel,int memory_directory,const unsigned char key[32],
                      const unsigned char receipt[80],int files[4],unsigned char challenge[32]);
/* Authenticated stage1: inputs verified; stage2: fixed-carrier delivery result.
 * Neither stage proves that an asynchronous upgrade reached its commit point. */
int pd_upload_reply(int channel,const unsigned char key[32],const unsigned char receipt[80],
                    const unsigned char challenge[32],unsigned stage,unsigned result);
#endif
