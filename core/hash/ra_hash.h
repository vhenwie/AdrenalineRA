#ifndef RA_HASH_H
#define RA_HASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RA_HASH_PATH_MAX 512

typedef struct {
    char path[RA_HASH_PATH_MAX];
    char hash[33];
    uint64_t file_size;
    uint64_t file_mtime;
    int from_cache;
} ra_hash_result_t;

/* Hash a PSP ISO, CSO or PBP with rcheevos rhash. If cache_path is non-NULL,
 * the result is cached by path + size + mtime. Returns 1 on success. */
int ra_hash_game(const char *path, const char *cache_path,
                 ra_hash_result_t *result);

/* Hash a PlayStation 1 disc with rc_hash_psx. Accepts a raw ISO, a CSO and a
 * POPS EBOOT.PBP (the PSISOIMG0000 deflate container is decoded on the fly).
 * If cache_path is non-NULL, the result is cached by path + size + mtime.
 * Returns 1 on success. */
int ra_hash_ps1(const char *path, const char *cache_path,
                ra_hash_result_t *result);

/* --- multi-disc -------------------------------------------------------------- */

/* Which disc of a multi-disc POPS EBOOT to hash: 0 = disc 1, 1 = disc 2, ... Set before
 * `ra_hash_ps1()` and reset to 0 after (single-threaded by design - hashing runs on the
 * runtime thread). */
void ra_hash_set_disc_index(int disc_index);
int ra_hash_get_disc_index(void);

/* Returns 1 when `path` is a POPS EBOOT.PBP carrying a PSISOIMG0000 disc image
 * (that is: a PS1 title Adrenaline runs through the POPS emulator). Reads only
 * the PBP header chain and the first disc block. */
int ra_hash_probe_ps1(const char *path);

/* Read the raw 10-byte PSP title ID from an ISO/CSO's LBA 16. */
int ra_hash_read_titleid(const char *path, char title_id[11]);

const char *ra_hash_last_error(void);

/* The container-level reason a PS1 read failed (block number, inflate code,
 * inflated size) and the parsed PBP geometry ("blocks=N index=0x.. data=0x..
 * size=N"). rcheevos' own message hides both, so the runtime logs them next to
 * it when a PS1 hash fails. */
const char *ra_hash_last_block_error(void);
const char *ra_hash_pbp_geometry(void);

/* ISO9660 hash of a POPS EBOOT using the walk documented in* (md5(boot name) ++ the boot file's bytes, length = PS-X EXE t_size + 0x800).
 * Exposed for the host harness; `ra_hash_ps1()` uses it when rc_hash_psx cannot
 * walk the container on device. Returns 1 on success. */
int ra_hash_ps1_from_container(const char *path, char hash[33]);

/* Boot payload position for the PS1 RAM gate: `offset` is `t_addr & 0x1FFFFF`,
 * `sample` the first 32 payload bytes (POPS strips the 0x800 header). Returns 0
 * when the container walk has not run for a PS-X EXE. */
int ra_hash_ps1_entry_point(uint32_t *offset, uint8_t sample[32]);

#ifdef __cplusplus
}
#endif

#endif /* RA_HASH_H */
