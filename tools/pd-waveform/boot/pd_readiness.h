#ifndef AETHER_PD_READINESS_H
#define AETHER_PD_READINESS_H
#include "pd_migration.h"
enum { PD_READINESS_SIZE=652, PD_READINESS_HTTP_MAX=69632 };
/* A local-only first installation uses empty address and the internal
 * local:<installation-id> tag in dns. The public API still reports no DNS name. */
struct pd_readiness { char hostname[64],address[16],dns[256]; };
/* Canonical zero-padded public identity, suitable for a durable worker record. */
int pd_readiness_valid(const struct pd_readiness* value);
/* Public identity only. A ready status is provisioned, running, unclaimable and
 * has no relay sessions. These checks do not prove the responding process's
 * identity: the worker must also pin/check the approved executable and listener. */
int pd_readiness_parse(const unsigned char* json,size_t size,struct pd_readiness* result);
int pd_readiness_http(const unsigned char* bytes,size_t size,struct pd_readiness* result);
/* Fixed GET /v1/status, port48992, at a canonical private IPv4 assigned locally.
 * No DNS, redirects, credentials, radio commands or caller-selected URL. */
int pd_readiness_probe(const char* local_address,struct pd_readiness* result);
int pd_readiness_equal(const struct pd_readiness* a,const struct pd_readiness* b);
/* At most15s of bounded HTTP attempts; succeeds only for the exact identity. */
int pd_readiness_wait(const char* local_address,const struct pd_readiness* expected);
/* Caller holds migration.lock. Immutable expected identity must be durable
 * before the predecessor is stopped; foreign records are preserved/refused. */
int pd_readiness_prepare(int store,const unsigned char plan[PD_MIGRATION_SIZE],const struct pd_readiness* expected);
int pd_readiness_read(int store,const unsigned char plan[PD_MIGRATION_SIZE],struct pd_readiness* result);
#endif
