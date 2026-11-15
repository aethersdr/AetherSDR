/* Permanent management entry; the selected payload cannot replace its gate. */
#define PD_FIXED_RUNTIME_DIRECTORY
#define inherited_shim_main pd_boot_primary_inherited_shim_main
#define PD_PRIMARY_MAIN pd_boot_primary
#include "../session/pd_primary_launch.c"
