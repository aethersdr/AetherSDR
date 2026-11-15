/* Permanent local-socket entry; arguments and executable names remain fixed. */
#define PD_FIXED_RUNTIME_DIRECTORY
#define PD_START_MAIN pd_boot_session
#include "../session/pd_start_session.c"
