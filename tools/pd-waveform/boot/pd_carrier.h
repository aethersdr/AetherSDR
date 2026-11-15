#ifndef AETHER_PD_CARRIER_H
#define AETHER_PD_CARRIER_H
/* Enter the host root delegated by the one-shot native sender. Requires host
 * PID/network context, root, and the fixed carrier capability set. No path or
 * command is accepted. On any failure the caller must exit without continuing.
 * Success retires SYS_CHROOT and enables no-new-privileges. ARM64 carrier only;
 * the Linux 2.6 native install route must not assume this interface exists. */
int pd_carrier_enter(int root);
#endif
