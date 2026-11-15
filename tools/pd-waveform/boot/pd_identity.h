#ifndef AETHER_PD_IDENTITY_H
#define AETHER_PD_IDENTITY_H
#include <stdint.h>
struct pd_identity {char model[16],firmware[28],serial[64],address[16];};
int pd_identity_parse(const char* reply,struct pd_identity* identity);
int pd_identity_probe(struct pd_identity* identity);
int pd_identity_matches(const struct pd_identity* identity,uint32_t profile,const char* firmware,const char* serial);
#endif
