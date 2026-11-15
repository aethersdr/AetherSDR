#ifndef AETHER_PD_WORKER_UNITS_H
#define AETHER_PD_WORKER_UNITS_H
#include "pd_transaction.h"
/* FLEX-8000 only. Caller holds the authenticated capsule lease and checks the
 * canonical unit directory. These functions publish/remove fixed definitions;
 * they never reload systemd or start a process. Arm recovery before migration. */
int pd_worker_units_arm(int units);
int pd_worker_units_verify(int units);
/* Remove only matching temporary definitions/enablement after durable terminal
 * state. The combined capsule cleanup calls this after executable detachment. */
int pd_worker_units_disarm(int units,int store,const unsigned char plan[PD_MIGRATION_SIZE]);
#endif
