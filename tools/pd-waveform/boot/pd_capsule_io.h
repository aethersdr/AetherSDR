#ifndef AETHER_PD_CAPSULE_IO_H
#define AETHER_PD_CAPSULE_IO_H
#include <stddef.h>
#include <sys/types.h>
/* Internal protected-file primitives shared by the fixed installer and upgrade
 * capsules. Callers authenticate source bytes and serialize a protected parent;
 * names are linked constants, never a privileged client request interface. */
int pd_capsule_io_directory(int fd,int private);
int pd_capsule_io_file(int fd,off_t size,mode_t mode);
int pd_capsule_io_read(int fd,unsigned char* bytes,size_t size,off_t offset);
int pd_capsule_io_hash(int fd,off_t limit,unsigned char digest[32],off_t* size);
int pd_capsule_io_equal(int fd,int source,const unsigned char* memory,off_t count);
int pd_capsule_io_existing(int directory,const char* name,int source,const unsigned char* memory,
                           off_t size,mode_t mode,off_t* present);
int pd_capsule_io_copy(int directory,const char* name,int source,const unsigned char* memory,off_t size,mode_t mode);
#endif
