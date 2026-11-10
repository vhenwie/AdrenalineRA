/*
	Adrenaline
	Copyright (C) 2016-2018, TheFloW

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __SCTRL_EXTERNS_H_
#define __SCTRL_EXTERNS_H_

#include <pspkermit.h>
#include <psprtc.h>
#include <pspkernel.h>

#include <systemctrl.h>
#include <systemctrl_se.h>
#include <rebootexconfig.h>
#include <systemctrl_epi.h>

#include "ra_gate.h"
#include "utils.h"

extern RebootexConfigEPI g_rebootex_config;
extern SEConfigEPI g_cfw_config;
extern STMOD_HANDLER g_module_handler;
extern SYSBOOT_HANDLER g_on_system_booted_handler;
extern SceAdrenaline *g_adrenaline;

/* Authenticated gate record, written by the Vita module immediately after its
 * SceAdrenaline struct. The PSP SDK struct ends at fake_api_type. */
static inline RaGateRecord *adrenaline_gate_record(void)
{
    return (RaGateRecord *)((u8 *)g_adrenaline + sizeof(SceAdrenaline));
}

static inline int adrenaline_gate_verify_now(void)
{
    u64 tick;
    RaGateRecord *record = adrenaline_gate_record();

    sceKernelDcacheInvalidateRange(record, sizeof(*record));
    sceRtcGetCurrentTick(&tick);
    return ra_gate_verify(record, (uint32_t)(tick / 1000000ULL),
                          RA_GATE_MAX_SKEW_SEC);
}

static inline void adrenaline_gate_writeback(void)
{
    sceKernelDcacheWritebackRange(adrenaline_gate_record(),
                                  sizeof(RaGateRecord));
}
_Static_assert(sizeof(SceAdrenaline) + sizeof(RaGateRecord) <= ADRENALINE_SIZE,
               "RA gate record must fit in the shared block");
extern u8 g_is_plugin_loading;

// Plugin used mem in bytes (text + data + stack)
extern SceSize g_plugins_loaded_mem;
extern SceSize g_last_plugin_mem_size;

extern int g_licensed_eboot;

void lowerString(char* orig, char* ret, int strSize);

int LoadExecForKernel_AA2029EC();

int sceChkregCheckRegion();
int sceChkregGetPsCode(u8 *pscode);

u32 sctrlHENFakeDevkitVersion();
SceUID sceIoOpenDrmPatched(const char *path, int flags, SceMode mode);

#endif /* __SCTRL_EXTERNS_H_ */