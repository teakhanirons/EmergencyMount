#include <taihen.h>

#include <string.h>
#include <stdio.h>

#include "blit/blit.h"

#include <psp2/usbstorvstor.h>
#include <psp2kern/power.h> 
#include <psp2kern/ctrl.h> 
#include <psp2kern/display.h>
#include <psp2kern/io/fcntl.h> 
#include <psp2kern/io/dirent.h> 
#include <psp2kern/udcd.h>
#include <psp2kern/sblaimgr.h> 
#include <psp2kern/kernel/cpu.h> 
#include <psp2kern/kernel/sysmem.h> 
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/modulemgr.h> 
#include <psp2kern/kernel/threadmgr.h> 
#include <psp2kern/kernel/dmac.h> 
#include "main.h"
#include "rikka.h"

tai_module_info_t vstorinfo;

static SceUID fb_uid = -1;
static SceUID hooks[3];

static tai_hook_ref_t ksceIoOpenRef;
static tai_hook_ref_t ksceIoReadRef;

SceCtrlData ctrl_peek, ctrl_press;

void *fb_addr = NULL;
char *path = NULL;
char *folder = NULL;
char menu[7][20] = {"Mount SD2Vita", "Mount Memory Card", "Mount PSVSD / USB", "Mount ur0:", "Reboot", "Shutdown", "Exit"};
int menusize;
int waifusize;
static int first = 1;
int select = 1;
int active = 0;
int PSTV = 0;

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define SCREEN_PITCH 1024
#define SCREEN_W 960
#define SCREEN_H 544

int (*setname)(const char *name, const char *version);
int (*setpath)(const char *path);
int (*activate)(SceUsbstorVstorType type);
int (*stop)(void);

int namesetter() { 
	int ret = 0;
	ret = setname("\"PS Vita\" MC", "1.00"); 
	return ret;
}

int pathsetter() { 
	int ret = 0;
	ret = setpath(path); 
	return ret;
}

int activator() { 
	int ret = 0;
	ret = activate(SCE_USBSTOR_VSTOR_TYPE_FAT);
	return ret;
}

int em_iofix(void *func) {
	int ret, state = 0;

	ENTER_SYSCALL(state);
	ret = em_iofix_threader(func);
	EXIT_SYSCALL(state);

	return ret;
}

int em_iofix_threader(void *func) { // can't access sdstor0: without a thread, this is a workaround
	int ret, res, uid = 0;
	ret = uid = ksceKernelCreateThread("em_iofix", func, 64, 0x10000, 0, 0, 0);
	if (ret < 0){ ret = -1; goto exit;}
	if ((ret = ksceKernelStartThread(uid, 0, NULL)) < 0) { ret = -1; goto exit;}
	if ((ret = ksceKernelWaitThreadEnd(uid, &res, NULL)) < 0) { ret = -1; goto exit;}
	ret = res;
exit:
	if (uid > 0) { ksceKernelDeleteThread(uid); }
	return ret;
}

static SceUID ksceIoOpenPatched(const char *file, int flags, SceMode mode) {
  first = 1;

  SceUID fd = TAI_CONTINUE(SceUID, ksceIoOpenRef, file, flags, mode);

  if (fd == 0x800F090D)
    return TAI_CONTINUE(SceUID, ksceIoOpenRef, file, flags & ~SCE_O_WRONLY, mode);

  return fd;
}

static int ksceIoReadPatched(SceUID fd, void *data, SceSize size) {
  int res = TAI_CONTINUE(int, ksceIoReadRef, fd, data, size);

  if (first) {
    first = 0;

    // Manipulate boot sector to support exFAT
    if (memcmp(data + 0x3, "EXFAT", 5) == 0) {
      // Sector size
      *(uint16_t *)(data + 0xB) = 1 << *(uint8_t *)(data + 0x6C);

      // Volume size
      *(uint32_t *)(data + 0x20) = *(uint32_t *)(data + 0x48);
    }
  }

  return res;
}

int pathCheck() {
  int fd = ksceIoOpen(path, SCE_O_RDONLY, 0777);
  if (fd < 0) {
  	ksceDebugPrintf("%s doesn't exist\n", path);
    return 0;
  }

  ksceIoClose(fd);
  ksceDebugPrintf("%s exists\n", path);
  return 1;
}

int sync() {
	ksceDebugPrintf("%s sync: %d\n", path, ksceIoSync(path, 0));
	return 1;
}

void drawScreen() {
	ksceDmacMemset(fb_addr, 0x00, SCREEN_PITCH * SCREEN_H * 4);
	if(PSTV) { 
		blit_stringf(20, 20, "EmergencyMount is only for PS Vita systems.");
		blit_stringf(20, 40, "Exiting now.");
		ksceKernelDelayThread(5*1000*1000);
	} else {
		blit_stringf(20, 20, "EmergencyMount by teakhanirons");
		blit_stringf(20, 40, "  Rikka Project by Team CBPS  ");
		blit_stringf(20, 60, "------------------------------");
		blit_stringf((strlen(menu[select - 1]) + 2) * 16, select * 20 + 60, "<");
		for(int i = 0; i < menusize; i++) { blit_stringf(20, ((i + 1) * 20) + 60, menu[i]); }
		for(int i = 0; i < waifusize; i++) { blit_stringf(400, ((i + 1) * 20) + 60, rikka[i]); }
	}
}

void StartUsb() {
	if(active == 0) {
	 	if(!path) {
			ksceDebugPrintf("path is null\n");
			return;
		}
  // Remove image path limitation
  	char zero[0x6E];
 	memset(zero, 0, 0x6E); // I can probably use DmacMemset here but I'll leave it as is since 0x6E bytes is small enough
  	hooks[0] = taiInjectDataForKernel(KERNEL_PID, vstorinfo.modid, 0, 0x1738, zero, 0x6E);

  // Add patches to support exFAT
  	hooks[1] = taiHookFunctionImportForKernel(KERNEL_PID, &ksceIoOpenRef, "SceUsbstorVStorDriver", 0x40FD29C7, 0x75192972, ksceIoOpenPatched);
  	hooks[2] = taiHookFunctionImportForKernel(KERNEL_PID, &ksceIoReadRef, "SceUsbstorVStorDriver", 0x40FD29C7, 0xE17EFC03, ksceIoReadPatched);
  	ksceDebugPrintf("exfat and image path limit patches = %x %x %x\n", hooks[0], hooks[1], hooks[2]);

  	ksceDebugPrintf("NAME: %x\n", em_iofix(namesetter));
  	ksceDebugPrintf("PATH: %x\n", em_iofix(pathsetter));
	ksceDebugPrintf("ACTI: %x\n", em_iofix(activator));
	active = 1;
	} else { ksceDebugPrintf("already activated\n"); }
}

void StopUsb() {
	if(active == 1) {
		active = 0;
		ksceDebugPrintf("STOP: %x\n", em_iofix(stop));
	} else { ksceDebugPrintf("usbstorvstor already stopped\n"); }
	ksceDebugPrintf("clear USB bus 2: %d\n", ksceUdcdStopCurrentInternal(2));
  	if (hooks[2] >= 0) { taiHookReleaseForKernel(hooks[2], ksceIoReadRef); }
	if (hooks[1] >= 0) { taiHookReleaseForKernel(hooks[1], ksceIoOpenRef); }
  	if (hooks[0] >= 0) { taiInjectReleaseForKernel(hooks[0]); }
  	em_iofix(sync);
}

int triaCheck() {
	SceCtrlData ctrl;

	ksceCtrlPeekBufferPositive(0, &ctrl, 1);

	ksceDebugPrintf("buttons held: 0x%08X\n", ctrl.buttons);

	if(ctrl.buttons & SCE_CTRL_TRIANGLE) {
		ksceDebugPrintf("WELCOME TO CHAOS WORLD!\n");
		return 1;
	} else {
		ksceDebugPrintf("triangle not held, exiting\n");
		return 0;
	}

}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	ksceDebugPrintf("\nEmergencyMount by Team CBPS\n----------\n");

	if(triaCheck() == 0) return SCE_KERNEL_START_SUCCESS;
	PSTV = ksceSblAimgrIsGenuineDolce();
if(!PSTV) {
	menusize = sizeof(menu) / sizeof(menu[0]);
	ksceDebugPrintf("menu size: %d\n", menusize);		
	waifusize = sizeof(rikka) / sizeof(rikka[0]);
	ksceDebugPrintf("waifu size: %d\n", waifusize);

  	vstorinfo.size = sizeof(tai_module_info_t);

  if(taiGetModuleInfoForKernel(KERNEL_PID, "SceUsbstorVStorDriver", &vstorinfo) < 0) {
  	ksceDebugPrintf("vstor not loaded\n");
  	return SCE_KERNEL_START_SUCCESS;
  }
  ksceDebugPrintf("vstor loaded\n");
  	
  	int rname = module_get_offset(KERNEL_PID, vstorinfo.modid, 0, 0x16b8 | 1, &setname);
  	int rpath = module_get_offset(KERNEL_PID, vstorinfo.modid, 0, 0x16d8 | 1, &setpath);
  	int racti = module_get_offset(KERNEL_PID, vstorinfo.modid, 0, 0x1710 | 1, &activate);
  	int rstop = module_get_offset(KERNEL_PID, vstorinfo.modid, 0, 0x1858 | 1, &stop);

  	ksceDebugPrintf("vstor hooks returns: %x %x %x %x\n", rname, rpath, racti, rstop);
} else { ksceDebugPrintf("I'M A PSTV\n"); }
	unsigned int fb_size = ALIGN(4 * SCREEN_PITCH * SCREEN_H, 256 * 1024);

	fb_uid = ksceKernelAllocMemBlock("fb", 0x40404006 , fb_size, NULL);

	ksceKernelGetMemBlockBase(fb_uid, &fb_addr);

	drawScreen();

	SceDisplayFrameBuf fb;
	fb.size        = sizeof(fb);
	fb.base        = fb_addr;
	fb.pitch       = SCREEN_PITCH;
	fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	fb.width       = SCREEN_W;
	fb.height      = SCREEN_H;

	ksceDisplaySetFrameBuf(&fb, 1);
	blit_set_frame_buf(&fb);

	drawScreen();

	while(!PSTV) {
		ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
		ctrl_press = ctrl_peek;
		ksceCtrlPeekBufferPositive(0, &ctrl_peek, 1);
		ctrl_press.buttons = ctrl_peek.buttons & ~ctrl_press.buttons;
			if(ctrl_press.buttons == SCE_CTRL_UP) {
				select = select - 1;
				if(select < 1) { select = menusize; }
				ksceDebugPrintf("select = %d\n", select);
				drawScreen();
			} else if(ctrl_press.buttons == SCE_CTRL_DOWN){
				select = select + 1;
				if(select > menusize) { select = 1; }
				ksceDebugPrintf("select = %d\n", select);
				drawScreen();
			} else if(ctrl_press.buttons == SCE_CTRL_CROSS || ctrl_press.buttons == SCE_CTRL_CIRCLE){
				ksceDebugPrintf("select = %d\n", select);
				if(select == 1) { //sd2vita
					path = "sdstor0:gcd-lp-ign-entire";
					if (em_iofix(pathCheck) == 0) {
						path = NULL;
						ksceDebugPrintf("---\n");
						continue; 
					}
				} else if(select == 2) { // sony mc
					path = "sdstor0:xmc-lp-ign-userext";
					if(em_iofix(pathCheck) == 0) { 
						path = "sdstor0:int-lp-ign-userext"; 
						if(em_iofix(pathCheck) == 0) { 
							path = NULL;
							ksceDebugPrintf("---\n");
							continue; 
						}
					}
				} else if(select == 3) { // psvsd / usb
					path = "sdstor0:uma-pp-act-a";
					if(em_iofix(pathCheck) == 0) { 
						path = "sdstor0:uma-lp-act-entire";
						if(em_iofix(pathCheck) == 0) { 
							path = NULL;
							ksceDebugPrintf("---\n");
							continue; 
						}
					} 
				} else if(select == 4) { // ur0
					path = "sdstor0:int-lp-ign-user";
					if(em_iofix(pathCheck) == 0) { 
						path = NULL;
						ksceDebugPrintf("---\n");
						continue; 
						}
				} else if(select == 5) { // reboot - kind of broken, sometimes doesnt reboot only shutdown
					StopUsb();
					ksceDebugPrintf("---\n");
					kscePowerRequestColdReset();
					ksceKernelDelayThread(5*1000*1000); //fallback
				} else if(select == 6) { // shutdown - may be broken?
					StopUsb();
					ksceDebugPrintf("---\n");
					kscePowerRequestStandby();
					ksceKernelDelayThread(5*1000*1000); //fallback
				} else if(select == 7) { // exit
					StopUsb();
					ksceDebugPrintf("---\n");
					break;
				}
				StopUsb();
      			StartUsb();
				ksceDebugPrintf("---\n");
			}
	};
	if (fb_uid) {
		ksceKernelFreeMemBlock(fb_uid);
	}

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	if (fb_uid) {
		ksceKernelFreeMemBlock(fb_uid);
	}

	return SCE_KERNEL_STOP_SUCCESS;
}