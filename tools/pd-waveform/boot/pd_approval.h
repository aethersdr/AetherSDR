#ifndef AETHER_PD_APPROVAL_H
#define AETHER_PD_APPROVAL_H
#include <stddef.h>
#include <stdint.h>
enum { PD_APPROVAL_SIZE=140 };
struct pd_approval {
    uint32_t profile;
    unsigned char digest[32];
    char firmware[28],serial[64];
};
int pd_approval_parse(const unsigned char* record,size_t size,struct pd_approval* approval);
int pd_approval_read(int fd,unsigned char record[PD_APPROVAL_SIZE],struct pd_approval* approval);
#endif
