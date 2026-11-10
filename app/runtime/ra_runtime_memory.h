#ifndef RA_RUNTIME_MEMORY_H
#define RA_RUNTIME_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "rc_client.h"

/* Maps the pspemu RAM block once. Returns 0 when the address converter is
 * missing or refused the mapping. */
int ra_runtime_memory_map(void);

/* Enables POPS reads once the PS1 boot payload is in RAM. */
void ra_runtime_memory_gate_tick(void);

/* Non-zero when the memory callbacks can answer a read right now. */
int ra_runtime_memory_readable(void);

/* rcheevos read callback: PS1 address space for POPS titles, PSP address space
 * otherwise. */
uint32_t ra_runtime_memory_read(uint32_t address, uint8_t *buffer,
                                uint32_t num_bytes, rc_client_t *client);

/* Reads a PSP address out of the mapped pspemu RAM. Returns 0 when the address
 * is outside the mapped window. */
int ra_runtime_memory_read_psp(uint32_t psp_address, uint8_t *buffer,
                               size_t bytes);

#endif /* RA_RUNTIME_MEMORY_H */
