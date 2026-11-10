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

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/devctl.h>
#include <psp2/io/stat.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <taihen.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pspdebug.h"

#include "../adrenaline_vita.h"

#define printf psvDebugScreenPrintf

#define EBOOT_URL "http://de01.psp.update.playstation.org/update/psp/image/eu/2014_1212_6be8878f475ac5b1a499b95ab2f7d301/EBOOT.PBP"
#define ADRENALINE_USER_AGENT "Adrenaline/1.00 libhttp/1.1"

static int downloadFile(const char *src, const char *dst) {
	int res;
	int statusCode;
	int tmplId = -1, connId = -1, reqId = -1;
	SceUID fd = -1;
	int ret = 1;

	res = sceHttpCreateTemplate(ADRENALINE_USER_AGENT, SCE_HTTP_VERSION_1_1, SCE_TRUE);
	if (res < 0)
		goto ERROR_EXIT;

	tmplId = res;

	res = sceHttpCreateConnectionWithURL(tmplId, src, SCE_TRUE);
	if (res < 0)
		goto ERROR_EXIT;

	connId = res;

	res = sceHttpCreateRequestWithURL(connId, SCE_HTTP_METHOD_GET, src, 0);
	if (res < 0)
		goto ERROR_EXIT;

	reqId = res;

	res = sceHttpSendRequest(reqId, NULL, 0);
	if (res < 0)
		goto ERROR_EXIT;

	res = sceHttpGetStatusCode(reqId, &statusCode);
	if (res < 0)
		goto ERROR_EXIT;

	if (statusCode == 200) {
		uint8_t buf[4096];
		uint64_t size = 0;
		uint32_t value = 0;

		res = sceHttpGetResponseContentLength(reqId, &size);
		if (res < 0)
			goto ERROR_EXIT;

		res = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
		if (res < 0)
			goto ERROR_EXIT;

		fd = res;

		printf("Downloading...");

		int x = psvDebugScreenGetX();
		int y = psvDebugScreenGetY();

		while (1) {
			int read = sceHttpReadData(reqId, buf, sizeof(buf));

			if (read < 0) {
				res = read;
				break;
			}

			if (read == 0)
				break;

			int written = sceIoWrite(fd, buf, read);

			if (written < 0) {
				res = written;
				break;
			}

			value += read;

			psvDebugScreenSetXY(x, y);
			printf("%d%%", (value * 100) / (uint32_t)size);
		}
	}

ERROR_EXIT:
	if (fd >= 0)
		sceIoClose(fd);

	if (reqId >= 0)
		sceHttpDeleteRequest(reqId);

	if (connId >= 0)
		sceHttpDeleteConnection(connId);

	if (tmplId >= 0)
		sceHttpDeleteTemplate(tmplId);

	if (res < 0)
		return res;

	return ret;
}

static void initNet() {
	static char memory[16 * 1024];

	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);

	SceNetInitParam param;
	param.memory = memory;
	param.size = sizeof(memory);
	param.flags = 0;

	sceNetInit(&param);
	sceNetCtlInit();

	sceHttpInit(40 * 1024);
}


static int copy_kd_module(const char *name, const char *dst) {
	uint8_t buf[4096];
	char src[192];
	SceUID in;
	SceUID out;
	int read;
	int written;
	int ok = 1;

	snprintf(src, sizeof(src), "ux0:app/" ADRENALINE_TITLEID "/%s", name);
	in = sceIoOpen(src, SCE_O_RDONLY, 0);
	if (in < 0)
		return 0;
	out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (out < 0) {
		sceIoClose(in);
		return 0;
	}
	while ((read = sceIoRead(in, buf, sizeof(buf))) > 0) {
		written = sceIoWrite(out, buf, read);
		if (written != read) {
			ok = 0;
			break;
		}
	}
	if (read < 0)
		ok = 0;
	sceIoClose(out);
	sceIoClose(in);
	return ok;
}

static void install_kd_modules(void) {
	static const char * const roots[] = {
		"ux0:data/" ADRENALINE_TITLEID,
		"ux0:app/" ADRENALINE_TITLEID
	};
	static const char * const modules[] = {
		"pentazemin.prx",
		"systemctrl.prx"
	};
	char flash0[128];
	char kd[160];
	char src[192];
	char dst[192];
	SceIoStat stat;
	SceIoStat source_stat;
	int i;
	int m;

	for (i = 0; i < 2; ++i) {
		snprintf(flash0, sizeof(flash0), "%s/flash0", roots[i]);
		if (sceIoGetstat(flash0, &stat) < 0)
			continue;
		snprintf(kd, sizeof(kd), "%s/kd", flash0);
		sceIoMkdir(kd, 0777);
		for (m = 0; m < 2; ++m) {
			snprintf(src, sizeof(src), "ux0:app/" ADRENALINE_TITLEID "/%s",
				modules[m]);
			if (sceIoGetstat(src, &source_stat) < 0)
				continue;
			snprintf(dst, sizeof(dst), "%s/%s", kd, modules[m]);
			if (sceIoGetstat(dst, &stat) >= 0 &&
				stat.st_size == source_stat.st_size)
				continue;
			if (!copy_kd_module(modules[m], dst))
				printf("Could not install %s to %s\n", modules[m], dst);
		}
	}
}

static int check_firmware_install() {
	SceIoStat stat;
	memset(&stat, 0, sizeof(SceIoStat));

	if (sceIoGetstat("ux0:data/" ADRENALINE_TITLEID, &stat) < 0) {
		sceIoMkdir("ux0:data/" ADRENALINE_TITLEID, 0777);
	}

	if (sceIoGetstat("ux0:data/" ADRENALINE_TITLEID "/flash0", &stat) < 0
		&& sceIoGetstat("ux0:data/" ADRENALINE_TITLEID "/661.PBP", &stat) < 0)
	{
		if (sceIoGetstat("ux0:app/" ADRENALINE_TITLEID "/flash0", &stat) < 0
			&& sceIoGetstat("ux0:app/" ADRENALINE_TITLEID "/661.PBP", &stat) < 0)
		{
			return 1;
		}
	}



	return 0;
}

int main() {
	int res;

	psvDebugScreenInit();

	// Safe mode
	if (sceIoDevctl("ux0:", 0x3001, NULL, 0, NULL, 0) == 0x80010030) {
		printf("Please enable unsafe homebrew first before using this software.");
		while (1);
	}

	// Enable write access
	sceAppMgrUmount("app0:");

	if (check_firmware_install()) {
		printf("The 6.61 firmware has not been installed yet and 661.PBP does not\nexist.\n");
		printf("Press X to download the PSP 6.61 firmware.\n");
		printf("Press any other button to ignore it (but you need to manually\nput 661.PBP to ux0:data/" ADRENALINE_TITLEID "/661.PBP or ux0:app/" ADRENALINE_TITLEID "/661.PBP" ").\n\n");

		while (1) {
			SceCtrlData pad;
			sceCtrlReadBufferPositive(0, &pad, 1);

			if (pad.buttons & SCE_CTRL_CROSS) {
				initNet();

				res = downloadFile(EBOOT_URL, "ux0:data/" ADRENALINE_TITLEID "/661.PBP");
				if (res < 0) {
					printf("Error 0x%08X downloading file.\n", res);
					while (1);
				}

				break;
			} else if (pad.buttons != 0) {
				break;
			}
		}
	}

	install_kd_modules();

	// Load kernel module
	res = taiLoadStartKernelModule("ux0:app/" ADRENALINE_TITLEID "/sce_module/adrenaline_kernel.skprx", 0, NULL, 0);
	if (res < 0) {
		printf("Could not load adrenaline_kernel.skprx. Please uninstall the old Adrenaline first.");
		while (1);
	}

    // TODO: delay and re-launch

	return 0;
}