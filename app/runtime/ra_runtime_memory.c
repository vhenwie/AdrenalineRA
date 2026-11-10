#include "ra_runtime_memory.h"

#include <string.h>

#include "main.h"
#include "ra_hash.h"
#include "ra_runtime_internal.h"

int ra_runtime_memory_map(void) {
  intptr_t mapped;

  if (g_rt.ram != NULL)
    return 1;
  if (ScePspemuConvertAddress == NULL) {
    ra_runtime_log("runtime: address converter unavailable\n");
    return 0;
  }
  mapped = (intptr_t)ScePspemuConvertAddress(RA_RUNTIME_RAM_BASE,
                         KERMIT_INPUT_MODE,
                         RA_RUNTIME_RAM_SIZE);
  if (mapped < 0x60000000 || mapped >= 0x80000000) {
    ra_runtime_log("runtime: RAM map rejected 0x%08X\n", (unsigned)mapped);
    return 0;
  }
  g_rt.ram = (uint8_t *)mapped;
  ra_runtime_log("runtime: RAM mapped 0x%08X\n", (unsigned)mapped);
  return 1;
}

/* POPS is readable only after the boot payload is in RAM; before that the window
 * holds the loader and a trigger evaluated against it submits garbage. The layout
 * mirrors DuckStation: 2 MB RAM followed by the 1 KB scratchpad. */
void ra_runtime_memory_gate_tick(void) {
  uint32_t offset;
  uint8_t sample[32];

  if (!g_rt.game_is_pops || g_rt.ps1_loaded || g_rt.ram == NULL)
    return;

  if (!ra_hash_ps1_entry_point(&offset, sample)) {
    /* A cached hash returns before the container walk, so the signature can be
     * missing even though the session is live. Capture it from the EBOOT once. */
    char ignored[33];

    if (g_rt.ps1_entry_tried)
      return;
    g_rt.ps1_entry_tried = 1;
    if (g_rt.game_filename[0] == '\0' ||
        !ra_hash_ps1_from_container(g_rt.game_filename, ignored) ||
        !ra_hash_ps1_entry_point(&offset, sample)) {
      ra_runtime_log("runtime: PS1 entry signature unavailable; PS1 reads stay "
                     "disabled\n");
      return;
    }
    ra_runtime_log("runtime: PS1 entry signature captured from the container\n");
  }

  if (offset > RA_POPS_RAM_SIZE - sizeof(sample))
    return;
  if (memcmp(g_rt.ram + RA_POPS_RAM_OFFSET + offset, sample, sizeof(sample)) != 0) {
    static int logged_wait;
    if (!logged_wait) {
      const uint8_t *ram = g_rt.ram + RA_POPS_RAM_OFFSET + offset;
      logged_wait = 1;
      ra_runtime_log("runtime: waiting for PS1 payload at 0x%X "
                     "(ram %02X%02X%02X%02X%02X%02X%02X%02X sample %02X%02X%02X%02X%02X%02X%02X%02X)\n",
                     (unsigned)offset, ram[0], ram[1], ram[2], ram[3], ram[4],
                     ram[5], ram[6], ram[7], sample[0], sample[1], sample[2],
                     sample[3], sample[4], sample[5], sample[6], sample[7]);
    }
    return;
  }

  g_rt.ps1_loaded = 1;
  if (g_rt.ps1_scratch == NULL && ScePspemuConvertAddress != NULL) {
    intptr_t scratch = ScePspemuConvertAddress(RA_POPS_SCRATCH_PSP,
                                               KERMIT_INPUT_MODE,
                                               RA_POPS_SCRATCH_SIZE);

    if (scratch >= 0x60000000 && scratch < 0x80000000)
      g_rt.ps1_scratch = (uint8_t *)scratch;
  }
  ra_runtime_log("runtime: PS1 payload found at 0x%X; PS1 reads enabled "
                 "(scratchpad %s)\n",
                 (unsigned)offset,
                 g_rt.ps1_scratch != NULL ? "mapped" : "unavailable");
}

/* Answers a read that cannot be served while evaluation is suspended. Returns the
 * number of bytes served, or 0 to leave the read as a failure.
 *
 * A short read here makes rcheevos disable every achievement and leaderboard that
 * shares the address, so a suspended read is answered with zeros at full length. */
static uint32_t runtime_read_while_suspended(uint32_t address, uint8_t *buffer,
                                             uint32_t num_bytes) {
  if (buffer == NULL || !ra_runtime_eval_suspended())
    return 0;

  memset(buffer, 0, num_bytes);
  if (g_rt.memory_zero_fills++ == 0) {
    ra_runtime_log("runtime: read %06X while evaluation is suspended; serving zeros "
                   "(a short read here makes rcheevos disable every achievement that "
                   "uses the address)\n",
                   (unsigned)address);
  }
  return num_bytes;
}

uint32_t ra_runtime_memory_read(uint32_t address, uint8_t *buffer,
                                uint32_t num_bytes, rc_client_t *client) {
  (void)client;
  if (!ra_runtime_game_memory_valid() || g_rt.ram == NULL || buffer == NULL) {
    ++g_rt.memory_refusals;
    return runtime_read_while_suspended(address, buffer, num_bytes);
  }
  if (g_rt.game_is_pops) {
    if (!g_rt.ps1_loaded) {
      /* The PS1 has not loaded the game yet; the window still holds the loader. */
      ++g_rt.memory_refusals;
      return runtime_read_while_suspended(address, buffer, num_bytes);
    }
    if ((uint64_t)address + num_bytes > RA_POPS_RAM_SIZE + RA_POPS_SCRATCH_SIZE) {
      ++g_rt.memory_refusals;
      return 0;
    }
    if (address >= RA_POPS_RAM_SIZE) {
      uint32_t scratch = address - RA_POPS_RAM_SIZE;
      if (g_rt.ps1_scratch == NULL) {
        ++g_rt.memory_refusals;
        return runtime_read_while_suspended(address, buffer, num_bytes);
      }
      memcpy(buffer, g_rt.ps1_scratch + scratch, num_bytes);
      return num_bytes;
    }
    memcpy(buffer, g_rt.ram + RA_POPS_RAM_OFFSET + address, num_bytes);
    return num_bytes;
  }
  if (address > RA_RUNTIME_RAM_SIZE ||
      num_bytes > RA_RUNTIME_RAM_SIZE - address) {
    /* Answer a bad peek with "no data" so rcheevos invalidates the value instead
     * of comparing garbage. */
    ++g_rt.memory_refusals;
    return 0;
  }
  memcpy(buffer, g_rt.ram + address, num_bytes);
  return num_bytes;
}

/* PSP address space, not the PS1 space answered above: the POPS container header
 * lives at PSP 0x09E80000, outside the 2 MB PS1 map. */
int ra_runtime_memory_read_psp(uint32_t psp_address, uint8_t *buffer,
                               size_t bytes) {
  uint32_t offset;

  if (g_rt.ram == NULL || buffer == NULL || bytes == 0)
    return 0;
  if (psp_address < RA_PSP_WINDOW_BASE)
    return 0;
  offset = psp_address - RA_PSP_WINDOW_BASE;
  if (offset > RA_RUNTIME_RAM_SIZE ||
      (uint64_t)bytes > (uint64_t)RA_RUNTIME_RAM_SIZE - (uint64_t)offset)
    return 0;
  memcpy(buffer, g_rt.ram + offset, bytes);
  return 1;
}

int ra_runtime_memory_readable(void) {
  if (!ra_runtime_game_memory_valid() || g_rt.ram == NULL)
    return 0;
  if (g_rt.game_is_pops && !g_rt.ps1_loaded)
    return 0;
  return 1;
}
