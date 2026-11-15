#ifndef AETHER_PD_RUNTIME_CONFIG_H
#define AETHER_PD_RUNTIME_CONFIG_H
enum { PD_CONFIG_ACCEPT=1,PD_CONFIG_DELIVER=2 };
/* Obtain the matching credential from the unprivileged shim, once per fixed
 * role. Public input contains only receipt and commitment. Return an owned,
 * read-only, unlinked tmpfs descriptor, or -1; never read an archive secret. */
int pd_runtime_config_fetch(int public_input,unsigned role);
#endif
