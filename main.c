#include <taihen.h>

#include <string.h>
#include <stdio.h>

#include "blit/blit.h"

#include <psp2kern/power.h> 
#include <psp2kern/ctrl.h> 
#include <psp2kern/display.h>
#include <psp2kern/io/fcntl.h> 
#include <psp2kern/io/dirent.h> 
#include <psp2kern/udcd.h>
#include <psp2kern/kernel/cpu.h> 
#include <psp2kern/kernel/sysmem.h> 
#include <psp2kern/kernel/modulemgr.h> 
#include <psp2kern/kernel/threadmgr.h> 

tai_module_info_t vstorinfo;

static SceUID fb_uid = -1;
static SceUID hooks[3];

static tai_hook_ref_t ksceIoOpenRef;
static tai_hook_ref_t ksceIoReadRef;

void *fb_addr = NULL;
char *path = NULL;
char menu[6][20] = {"Mount SD2Vita", "Mount Memory Card", "Mount PSVSD / USB", "Mount ur0:", "Exit", "CBPS"};
int menusize;
static int first = 1;
int select = 1;
int active = 0;

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define SCREEN_PITCH 1024
#define SCREEN_W 960
#define SCREEN_H 544

typedef enum SceUsbstorVstorType {
	SCE_USBSTOR_VSTOR_TYPE_FAT     = 0,
	SCE_USBSTOR_VSTOR_TYPE_CDROM   = 5
} SceUsbstorVstorType;

int (*setname)(const char *name, const char *version);
int (*setpath)(const char *path);
int (*activate)(SceUsbstorVstorType type);
int (*stop)(void);
int (*mtpstart)(int flags);

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

void drawScreen() {
	for (int i = 0; i < 544; i++) {
		for (int j = 0; j < 960; j++) {
			((unsigned int *)fb_addr)[j + i * SCREEN_PITCH] = 0xFF000000;
		}
	}
	blit_stringf(320, select * 20, "<");
	for(int i = 0; i < menusize; i++) { blit_stringf(20, ((i + 1) * 20), menu[i]); }
}

void StartUsb() {
	if(active == 0) {
	 	if(!path) {
			ksceDebugPrintf("path is null\n");
			return;
		}
  // Remove image path limitation
  	char zero[0x6E];
 	memset(zero, 0, 0x6E);
  	hooks[0] = taiInjectDataForKernel(KERNEL_PID, vstorinfo.modid, 0, 0x1738, zero, 0x6E);

  // Add patches to support exFAT
  	hooks[1] = taiHookFunctionImportForKernel(KERNEL_PID, &ksceIoOpenRef, "SceUsbstorVStorDriver", 0x40FD29C7, 0x75192972, ksceIoOpenPatched);
  	hooks[2] = taiHookFunctionImportForKernel(KERNEL_PID, &ksceIoReadRef, "SceUsbstorVStorDriver", 0x40FD29C7, 0xE17EFC03, ksceIoReadPatched);
  	ksceDebugPrintf("exfat and image path limit patches = %x %x %x\n", hooks[0], hooks[1], hooks[2]);

  	if(ksceUdcdStopCurrentInternal(2) == 0) { ksceDebugPrintf("cleared USB bus 2\n"); }
  	if(em_iofix(setname("\"PS Vita\" MC", "1.00")) == 0) { ksceDebugPrintf("name set\n"); }
  	if(em_iofix(setpath(path)) == 0) { ksceDebugPrintf("path set\n"); }
		active = 1;
		if(activate(SCE_USBSTOR_VSTOR_TYPE_FAT) == 0) { ksceDebugPrintf("activated mount\n"); }
	} else { ksceDebugPrintf("already activated\n"); }
}

void StopUsb() {
	if(active == 1) {
		active = 0;
		if(em_iofix(stop) == 0) { ksceDebugPrintf("stopped mount\n"); }
		if(em_iofix(mtpstart(1)) == 0) { ksceDebugPrintf("restarted MTP\n"); }
	} else { ksceDebugPrintf("already stopped\n"); }
  	if (hooks[2] >= 0) { taiHookReleaseForKernel(hooks[2], ksceIoReadRef); }
	if (hooks[1] >= 0) { taiHookReleaseForKernel(hooks[1], ksceIoOpenRef); }
  	if (hooks[0] >= 0) { taiInjectReleaseForKernel(hooks[0]); }
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

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {

	for(int i = 0; i < 100; i++) {
		if(menu[i] != menu[5]) { menusize = menusize + 1; }
		else { break; }
	}
	ksceDebugPrintf("menu size: %d\n", menusize);
  	vstorinfo.size = sizeof(tai_module_info_t);

  if(taiGetModuleInfoForKernel(KERNEL_PID, "SceUsbstorVStorDriver", &vstorinfo) < 0) {
  	ksceDebugPrintf("vstor not loaded\n");
  	return SCE_KERNEL_START_SUCCESS;
  }
  	ksceDebugPrintf("vstor loaded\n");
  	module_get_export_func(KERNEL_PID, "SceUsbstorVStorDriver", 0x17F294B9, 0x14455C20, &setname);
  	module_get_export_func(KERNEL_PID, "SceUsbstorVStorDriver", 0x17F294B9, 0x8C9F93AB, &setpath);
  	module_get_export_func(KERNEL_PID, "SceUsbstorVStorDriver", 0x17F294B9, 0xB606F1AF, &activate);
  	module_get_export_func(KERNEL_PID, "SceUsbstorVStorDriver", 0x17F294B9, 0x0FD67059, &stop);
  	module_get_export_func(KERNEL_PID, "SceMtpIfDriver", 0x5EFA4138, 0xC5327CAC, &mtpstart);

  	ksceDebugPrintf("vstor hooks: %x %x %x %x\n", &setname, &setpath, &activate, &stop);
  	ksceDebugPrintf("mtp hook: %x\n", &mtpstart);

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
	blit_set_color(0x00FFFFFF, 0xFF000000);
	drawScreen();

	SceCtrlData ctrl_peek, ctrl_press;
	while(1) {
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
					// path = uma
				} else if(select == 5) { // exit
					StopUsb();
					ksceDebugPrintf("---\n");
					break;
				}
				StopUsb();
      			StartUsb();
				ksceDebugPrintf("---\n");
			}
	};

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	if (fb_uid) {
		ksceKernelFreeMemBlock(fb_uid);
	}

	return SCE_KERNEL_STOP_SUCCESS;
}