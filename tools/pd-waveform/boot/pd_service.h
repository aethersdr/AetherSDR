#ifndef AETHER_PD_SERVICE_H
#define AETHER_PD_SERVICE_H
#include "pd_primary.h"
#include "pd_migration.h"
#include "pd_upgrade.h"
enum pd_service_kind { PD_SERVICE_BOOT=0,PD_SERVICE_SOCKET=1,PD_SERVICE_PRIMARY=2,PD_SERVICE_SESSION=3 };
enum { PD_SERVICE_OUTPUT_MAX=65536,PD_SERVICE_SESSIONS_MAX=32 };
struct pd_service_state { pid_t pid,control_pid;int active,quiet,managed_startup,guardian_hold,populated,failed,control_restart; };
/* Fixed-unit adapter for authenticated installation transactions or the
 * peer-verified root socket entry. No client-supplied unit/path/arguments. */
int pd_service_parse(unsigned kind,const unsigned char* bytes,size_t size,struct pd_service_state* state);
struct pd_service_sessions {
    unsigned count,retire_count,record_count;
    int managed_startup,guardian_hold;
    char retire_names[PD_SERVICE_SESSIONS_MAX][96];
    char names[PD_SERVICE_SESSIONS_MAX][96];
    struct pd_service_state states[PD_SERVICE_SESSIONS_MAX];
};
int pd_service_sessions_parse(const unsigned char* bytes,size_t size,struct pd_service_sessions* sessions);
/* 1 owned, 0 unrelated, -1 malformed. Kernel cgroup metadata only. */
int pd_service_owned_cgroup(const unsigned char* bytes,size_t size);
int pd_service_runtime_quiet(void);
int pd_service_quiesce(void);
int pd_service_files(void);
int pd_service_read(unsigned kind,struct pd_service_state* state);
/* Caller holds the capsule namespace lease. Active/exited boot and an active
 * socket with no ExecStartPost process exclude older cleanup already past its
 * lease. Later boot/socket-owner invocations cannot acquire that held lease. */
int pd_service_upgrade_cleanup_idle(void);
int pd_service_reload(void);
/* Fresh applied metadata must enforce the current session guardian hold. */
int pd_service_session_policy(void);
/* Fixed root socket entry only: clears bounded, attested empty failure records. */
int pd_service_session_prepare(void);
int pd_service_prepare_runtime(int store,const unsigned char plan[PD_MIGRATION_SIZE]);
int pd_service_activate(int store,const unsigned char plan[PD_MIGRATION_SIZE],int executable,
                        const char* address,const struct pd_readiness* expected,struct pd_primary* primary);
/* Upgrade activation/restoration uses the same fixed commands and process
 * attestation, gated by its matching journal and live exclusive worker. */
int pd_service_upgrade_prepare_runtime(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE]);
int pd_service_upgrade_activate(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],int executable,
                                const char* address,const struct pd_readiness* expected,struct pd_primary* primary);
/* Stops only the primary, after verifying its loaded definition. Full session
 * graph quiescence is a separate rollback obligation; this does not claim it. */
int pd_service_stop_primary(void);
/* The independent worker must be the verified MainPID of its fixed unit;
 * the other worker unit must be quiet before any transaction action. */
int pd_service_worker_parse(int recovery,const unsigned char* bytes,size_t size,struct pd_service_state* state);
int pd_service_worker_ready(int units,int recovery);
int pd_service_worker_launch(int units);
int pd_service_upgrade_worker_parse(int recovery,const unsigned char* bytes,size_t size,struct pd_service_state* state);
int pd_service_upgrade_worker_ready(int units,int recovery);
int pd_service_upgrade_worker_launch(int units);
/* Publish/remove only the fixed primary boot link, syncing its directory. */
int pd_service_primary_autostart(int units,int enabled);
#endif
