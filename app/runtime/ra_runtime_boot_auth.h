#ifndef RA_RUNTIME_BOOT_AUTH_H
#define RA_RUNTIME_BOOT_AUTH_H

/* Boot-time login on the runtime worker: skipped when disabled, offline or without
 * credentials; otherwise creates the client and pumps it until login lands. */
void ra_runtime_boot_auth(void);

#endif /* RA_RUNTIME_BOOT_AUTH_H */
