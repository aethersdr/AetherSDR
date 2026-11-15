#ifndef AETHER_PD_INSTALLATION_H
#define AETHER_PD_INSTALLATION_H
/* Public, root-published terminal transaction summary. No credential or authority.
 * Eight-byte magic, phase/profile u32, plan/release/key digests, firmware/serial. */
enum { PD_INSTALLATION_SIZE=204 };
/* Acquire a shared terminal transaction lease and verify its protected selector.
 * Call only after the boot/worker authenticated and activated the selected payload.
 * The runtime mirror is atomic and may be absent; it never drives installation. */
int pd_installation_publish(int store,int runtime);
int pd_installation_publish_runtime(int store);
#endif
