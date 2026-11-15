#ifndef AETHER_PD_STORE_H
#define AETHER_PD_STORE_H
#include <stdint.h>
enum { PD_SELECTOR_SIZE=88, PD_STORE_RESERVE=8*1024*1024 };
struct pd_selection {
    unsigned char current,previous;
    unsigned char current_hash[32],previous_hash[32];
};
/* Callers own protected directory descriptors and hold the store lock.
 * No package-selected paths are used by these operations. */
int pd_selection_read(int store,struct pd_selection* selected);
int pd_store_slot(int slots,unsigned char name);
int pd_store_lock(int store);
int pd_store_update(int store,int input,const unsigned char key[32],uint32_t arch);
int pd_store_rollback(int store,const unsigned char key[32],uint32_t arch);
#endif
