#ifndef AETHER_PD_TRANSPORT_H
#define AETHER_PD_TRANSPORT_H
#include <sys/types.h>
enum { PD_TRANSPORT_FDS=5, PD_TRANSPORT_RECEIPT=80 };
/* Private 120-byte PDXFER01 configuration, owner-only and read-only. The caller
 * supplies the expected local owner; the file is produced only after consent. */
int pd_transport_config(int fd,uid_t owner,unsigned char key[32],unsigned char receipt[80]);
/* Owned connected AF_UNIX/SOCK_SEQPACKET sockets only. Both sides independently know
 * the private request key and exact approved receipt. Descriptor order is host
 * root, envelope, plan, candidate, and previous release (upgrade only).
 * Receiving authenticates the peer and descriptor shape, not release contents;
 * pd_delivery_run must still verify signatures and the live target before use.
 * Adopts blocking mode with bounded I/O timeouts. Receive closes every received
 * descriptor on failure. No pathname/command ABI. */
int pd_transport_send(int socket,uid_t receiver,const unsigned char key[32],
    const unsigned char receipt[PD_TRANSPORT_RECEIPT],const int files[PD_TRANSPORT_FDS]);
int pd_transport_receive(int socket,uid_t sender,const unsigned char key[32],
    const unsigned char receipt[PD_TRANSPORT_RECEIPT],int files[PD_TRANSPORT_FDS]);
/* Completion travels on the authenticated owned relay socket, never via logs. */
int pd_transport_result_send(int socket,int result);
int pd_transport_result_receive(int socket,int* result);
#endif
