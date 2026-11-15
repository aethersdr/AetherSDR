#ifndef AETHER_PD_ENDPOINT_H
#define AETHER_PD_ENDPOINT_H
/* Public abstract socket names identify the envelope, not the request secret.
 * Authentication remains mandatory after connecting. */
int pd_endpoint_listen(const unsigned char receipt[80],int relay);
#endif
