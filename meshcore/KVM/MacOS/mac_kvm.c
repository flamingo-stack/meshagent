/*
Copyright 2010 - 2018 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "mac_kvm.h"
#include "../../meshdefines.h"
#include "../../meshinfo.h"
#include "../../../microstack/ILibParsers.h"
#include "../../../microstack/ILibAsyncSocket.h"
#include "../../../microstack/ILibAsyncServerSocket.h"
#include "../../../microstack/ILibProcessPipe.h"
#include <IOKit/IOKitLib.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreServices/CoreServices.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <string.h>
#include <pwd.h>
#include <sys/stat.h>

int KVM_Listener_FD = -1;
#define KVM_Listener_Path "/usr/local/mesh_services/meshagent/kvm"

// LaunchAgent KVM mode - daemon connects to agent's socket instead of spawning child
#define USE_LAUNCHAGENT_KVM 1
#define KVM_LAUNCHAGENT_SOCKET_PATH "/tmp/meshkvm_agent.sock"
int g_kvm_socket_fd = -1;           // Socket fd for daemon<->agent communication
void *g_kvm_socket_user = NULL;     // User data for socket mode callbacks
pthread_t g_socket_read_thread;     // Thread for reading from agent socket
int g_socket_read_running = 0;      // Flag to control read thread
#if defined(_TLSLOG)
#define TLSLOG1 printf
#else
#define TLSLOG1(...) ;
#endif


int KVM_AGENT_FD = -1;
int KVM_SEND(char *buffer, int bufferLen)
{
	int retVal = -1;
	retVal = write(KVM_AGENT_FD == -1 ? STDOUT_FILENO : KVM_AGENT_FD, buffer, bufferLen);
	if (KVM_AGENT_FD == -1) { fsync(STDOUT_FILENO); }
	else
	{
		if (retVal < 0)
		{
			char tmp[255];
			int tmpLen = sprintf_s(tmp, sizeof(tmp), "Write Error: %d on %d\n", errno, KVM_AGENT_FD);
			write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
		}
	}
	return(retVal);
}



CGDirectDisplayID SCREEN_NUM = 0;
int SH_HANDLE = 0;
int SCREEN_WIDTH = 0;
int SCREEN_HEIGHT = 0;
int SCREEN_SCALE = 1;
int SCREEN_SCALE_SET = 0;
int SCREEN_DEPTH = 0;
int TILE_WIDTH = 0;
int TILE_HEIGHT = 0;
int TILE_WIDTH_COUNT = 0;
int TILE_HEIGHT_COUNT = 0;
int COMPRESSION_RATIO = 0;
int FRAME_RATE_TIMER = 0;
struct tileInfo_t **g_tileInfo = NULL;
int g_remotepause = 0;
int g_pause = 0;
int g_shutdown = 0;
int g_cleanup_in_progress = 0;  // Guard against double cleanup
int g_openFrameMode = 0;        // OpenFrame mode - enables KVM child process reuse
int g_resetipc = 0;
int kvm_clientProcessId = 0;
int g_restartcount = 0;
int g_totalRestartCount = 0;
int restartKvm = 0;
extern void* tilebuffer;
pid_t g_slavekvm = 0;
pthread_t kvmthread = (pthread_t)NULL;
ILibProcessPipe_Process gChildProcess;
ILibQueue g_messageQ;

//int logenabled = 1;
//FILE *logfile = NULL;
//#define MASTERLOGFILE "/dev/null"
//#define SLAVELOGFILE "/dev/null"
//#define LOGFILE "/dev/null"


#define KvmDebugLog(...)
//#define KvmDebugLog(...) printf(__VA_ARGS__); if (logfile != NULL) fprintf(logfile, __VA_ARGS__);
//#define KvmDebugLog(x) if (logenabled) printf(x);
//#define KvmDebugLog(x) if (logenabled) fprintf(logfile, "Writing from slave in kvm_send_resolution\n");

void senddebug(int val)
{
	char *buffer = (char*)ILibMemory_SmartAllocate(8);

	((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_DEBUG);	// Write the type
	((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)8);			// Write the size
	((int*)buffer)[1] = val;

	ILibQueue_Lock(g_messageQ);
	ILibQueue_EnQueue(g_messageQ, buffer);
	ILibQueue_UnLock(g_messageQ);
}



void kvm_send_resolution() 
{
	char *buffer = ILibMemory_SmartAllocate(8);
	
	((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_SCREEN);	// Write the type
	((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)8);				// Write the size
	((unsigned short*)buffer)[2] = (unsigned short)htons((unsigned short)SCREEN_WIDTH);		// X position
	((unsigned short*)buffer)[3] = (unsigned short)htons((unsigned short)SCREEN_HEIGHT);	// Y position


	// Write the reply to the pipe.
	ILibQueue_Lock(g_messageQ);
	ILibQueue_EnQueue(g_messageQ, buffer);
	ILibQueue_UnLock(g_messageQ);
}

#define BUFSIZE 65535

int set_kbd_state(int input_state)
{
	int ret = 0;
	kern_return_t kr;
	io_service_t ios;
	io_connect_t ioc;
	CFMutableDictionaryRef mdict;

	while (1)
	{
		mdict = IOServiceMatching(kIOHIDSystemClass);
		ios = IOServiceGetMatchingService(kIOMasterPortDefault, (CFDictionaryRef)mdict);
		if (!ios)
		{
			if (mdict)
			{
				CFRelease(mdict);
			}
			ILIBLOGMESSAGEX("IOServiceGetMatchingService() failed\n");
			break;
		}

		kr = IOServiceOpen(ios, mach_task_self(), kIOHIDParamConnectType, &ioc);
		IOObjectRelease(ios);
		if (kr != KERN_SUCCESS)
		{
			ILIBLOGMESSAGEX("IOServiceOpen() failed: %x\n", kr);
			break;
		}

		// Set CAPSLOCK
		kr = IOHIDSetModifierLockState(ioc, kIOHIDCapsLockState, (input_state & 4) == 4);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}

		// Set NUMLOCK
		kr = IOHIDSetModifierLockState(ioc, kIOHIDNumLockState, (input_state & 1) == 1);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}

		// CAPSLOCK_QUERY
		bool state;
		kr = IOHIDGetModifierLockState(ioc, kIOHIDCapsLockState, &state);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}
		ret |= (state << 2);

		// NUMLOCK_QUERY
		kr = IOHIDGetModifierLockState(ioc, kIOHIDNumLockState, &state);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}
		ret |= state;

		IOServiceClose(ioc);
		break;
	}
	return(ret);
}
int get_kbd_state()
{
	int ret = 0;
	kern_return_t kr;
	io_service_t ios;
	io_connect_t ioc;
	CFMutableDictionaryRef mdict;

	while (1)
	{
		mdict = IOServiceMatching(kIOHIDSystemClass);
		ios = IOServiceGetMatchingService(kIOMasterPortDefault, (CFDictionaryRef)mdict);
		if (!ios)
		{
			if (mdict)
			{
				CFRelease(mdict);
			}
			ILIBLOGMESSAGEX("IOServiceGetMatchingService() failed\n");
			break;
		}

		kr = IOServiceOpen(ios, mach_task_self(), kIOHIDParamConnectType, &ioc);
		IOObjectRelease(ios);
		if (kr != KERN_SUCCESS)
		{
			ILIBLOGMESSAGEX("IOServiceOpen() failed: %x\n", kr);
			break;
		}

		// CAPSLOCK_QUERY
		bool state;
		kr = IOHIDGetModifierLockState(ioc, kIOHIDCapsLockState, &state);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}
		ret |= (state << 2);

		// NUMLOCK_QUERY
		kr = IOHIDGetModifierLockState(ioc, kIOHIDNumLockState, &state);
		if (kr != KERN_SUCCESS)
		{
			IOServiceClose(ioc);
			ILIBLOGMESSAGEX("IOHIDGetModifierLockState() failed: %x\n", kr);
			break;
		}
		ret |= state;

		IOServiceClose(ioc);
		break;
	}
	return(ret);
}


int kvm_init()
{
	ILibCriticalLogFilename = "KVMSlave.log";
	int old_height_count = TILE_HEIGHT_COUNT;
	
	SCREEN_NUM = CGMainDisplayID();
	
	if (SCREEN_WIDTH > 0)
	{
		CGDisplayModeRef mode = CGDisplayCopyDisplayMode(SCREEN_NUM);
		SCREEN_SCALE = (int) CGDisplayModeGetPixelWidth(mode) / SCREEN_WIDTH;
		CGDisplayModeRelease(mode);
	}

	SCREEN_HEIGHT = CGDisplayPixelsHigh(SCREEN_NUM) * SCREEN_SCALE;
	SCREEN_WIDTH = CGDisplayPixelsWide(SCREEN_NUM) * SCREEN_SCALE;
	// Some magic numbers.
	TILE_WIDTH = 32;
	TILE_HEIGHT = 32;
	COMPRESSION_RATIO = 50;
	FRAME_RATE_TIMER = 100;
	
	TILE_HEIGHT_COUNT = SCREEN_HEIGHT / TILE_HEIGHT;
	TILE_WIDTH_COUNT = SCREEN_WIDTH / TILE_WIDTH;
	if (SCREEN_WIDTH % TILE_WIDTH) { TILE_WIDTH_COUNT++; }
	if (SCREEN_HEIGHT % TILE_HEIGHT) { TILE_HEIGHT_COUNT++; }
	
	kvm_send_resolution();
	reset_tile_info(old_height_count);
	
	unsigned char *buffer = ILibMemory_SmartAllocate(5);
	((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_KEYSTATE);		// Write the type
	((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)5);					// Write the size
	buffer[4] = (unsigned char)get_kbd_state();

	// Write the reply to the pipe.
	ILibQueue_Lock(g_messageQ);
	ILibQueue_EnQueue(g_messageQ, buffer);
	ILibQueue_UnLock(g_messageQ);
	return 0;
}

// void CheckDesktopSwitch(int checkres) { return; }

int kvm_server_inputdata(char* block, int blocklen)
{
	unsigned short type, size;
	//CheckDesktopSwitch(0);
	
	//senddebug(100+blocklen);

	// Decode the block header
	if (blocklen < 4) return 0;
	type = ntohs(((unsigned short*)(block))[0]);
	size = ntohs(((unsigned short*)(block))[1]);

	if (size > blocklen) return 0;

	switch (type)
	{
		case MNG_KVM_KEY_UNICODE: // Unicode Key
			if (size != 7) break;
			KeyActionUnicode(((((unsigned char)block[5]) << 8) + ((unsigned char)block[6])), block[4]);
			break;
		case MNG_KVM_KEY: // Key
		{
			if (size != 6 || KVM_AGENT_FD != -1) { break; }
			KeyAction(block[5], block[4]);
			break;
		}
		case MNG_KVM_MOUSE: // Mouse
		{
			int x, y;
			short w = 0;
			if (KVM_AGENT_FD != -1) { break; }
			if (size == 10 || size == 12)
			{
				x = ((int)ntohs(((unsigned short*)(block))[3])) / SCREEN_SCALE;
				y = ((int)ntohs(((unsigned short*)(block))[4])) / SCREEN_SCALE;
				
				if (size == 12) w = ((short)ntohs(((short*)(block))[5]));
				
				//printf("x:%d, y:%d, b:%d, w:%d\n", x, y, block[5], w);
				MouseAction(x, y, (int)(unsigned char)(block[5]), w);
			}
			break;
		}
		case MNG_KVM_COMPRESSION: // Compression
		{
			if (size != 6) break;
			set_tile_compression((int)block[4], (int)block[5]);
			COMPRESSION_RATIO = 100;
			break;
		}
		case MNG_KVM_REFRESH: // Refresh
		{
			fprintf(stderr, "[KVM-CHILD] REFRESH received, g_shutdown=%d\n", g_shutdown);
			fflush(stderr);

			kvm_send_resolution();

			int row, col;
			if (size != 4) break;
			if (g_tileInfo == NULL) {
				if ((g_tileInfo = (struct tileInfo_t **) malloc(TILE_HEIGHT_COUNT * sizeof(struct tileInfo_t *))) == NULL) ILIBCRITICALEXIT(254);
				for (row = 0; row < TILE_HEIGHT_COUNT; row++) {
					if ((g_tileInfo[row] = (struct tileInfo_t *) malloc(TILE_WIDTH_COUNT * sizeof(struct tileInfo_t))) == NULL) ILIBCRITICALEXIT(254);
				}
			}
			for (row = 0; row < TILE_HEIGHT_COUNT; row++) {
				for (col = 0; col < TILE_WIDTH_COUNT; col++) {
					g_tileInfo[row][col].crc = 0xFF;
					g_tileInfo[row][col].flag = 0;
				}
			}
			fprintf(stderr, "[KVM-CHILD] REFRESH processed, tiles reset\n");
			fflush(stderr);
			break;
		}
		case MNG_KVM_PAUSE: // Pause
		{
			if (size != 5) break;
			fprintf(stderr, "[KVM-CHILD] PAUSE received, value=%d\n", block[4]);
			fflush(stderr);
			g_remotepause = block[4];
			break;
		}
		case MNG_KVM_FRAME_RATE_TIMER:
		{
			//int fr = ((int)ntohs(((unsigned short*)(block))[2]));
			//if (fr > 20 && fr < 2000) FRAME_RATE_TIMER = fr;
			break;
		}
	}

	return size;
}


static int g_feeddata_count = 0;  // Counter for feeddata calls

int kvm_relay_feeddata(char* buf, int len)
{
	g_feeddata_count++;
#if USE_LAUNCHAGENT_KVM
	// In LaunchAgent mode, send via socket if connected
	if (g_openFrameMode && g_kvm_socket_fd >= 0)
	{
		if (g_feeddata_count <= 5 || g_feeddata_count % 100 == 0)
		{
			fprintf(stderr, "[KVM-FEEDDATA] #%d: Using socket mode, len=%d, fd=%d\n",
				g_feeddata_count, len, g_kvm_socket_fd);
			fflush(stderr);
		}
		return kvm_socket_send(buf, len);
	}
	if (g_feeddata_count <= 5)
	{
		fprintf(stderr, "[KVM-FEEDDATA] #%d: Using pipe mode (g_openFrameMode=%d, g_kvm_socket_fd=%d), len=%d\n",
			g_feeddata_count, g_openFrameMode, g_kvm_socket_fd, len);
		fflush(stderr);
	}
#endif
	ILibProcessPipe_Process_WriteStdIn(gChildProcess, buf, len, ILibTransport_MemoryOwnership_USER);
	return(len);
}

// Set the KVM pause state
void kvm_pause(int pause)
{
	g_pause = pause;
}


void* kvm_mainloopinput(void* param)
{
	int ptr = 0;
	int ptr2 = 0;
	int len = 0;
	char* pchRequest2[30000];
	int cbBytesRead = 0;

	char tmp[255];
	int tmpLen;

	if (KVM_AGENT_FD == -1)
	{
		int flags;
		flags = fcntl(STDIN_FILENO, F_GETFL, 0);
		if (fcntl(STDIN_FILENO, F_SETFL, (O_NONBLOCK | flags) ^ O_NONBLOCK) == -1) { senddebug(-999); }
	}

	while (!g_shutdown)
	{
		if (KVM_AGENT_FD != -1)
		{
			tmpLen = sprintf_s(tmp, sizeof(tmp), "About to read from IPC Socket\n");
			write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
		}

		KvmDebugLog("Reading from master in kvm_mainloopinput\n");
		cbBytesRead = read(KVM_AGENT_FD == -1 ? STDIN_FILENO: KVM_AGENT_FD, pchRequest2 + len, 30000 - len);
		KvmDebugLog("Read %d bytes from master in kvm_mainloopinput\n", cbBytesRead);

		if (KVM_AGENT_FD != -1)
		{
			tmpLen = sprintf_s(tmp, sizeof(tmp), "Read %d bytes from IPC-xx-Socket\n", cbBytesRead);
			write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
		}

		if (cbBytesRead == -1 || cbBytesRead == 0) 
		{ 
			/*ILIBMESSAGE("KVMBREAK-K1\r\n"); g_shutdown = 1; printf("shutdown\n");*/ 
			if (KVM_AGENT_FD == -1)
			{
				g_shutdown = 1;
			}
			else
			{
				g_resetipc = 1;
			}
			break; 
		}
		len += cbBytesRead;
		ptr2 = 0;
		
		if (KVM_AGENT_FD != -1)
		{
			tmpLen = sprintf_s(tmp, sizeof(tmp), "enter while\n");
			write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
		}
		while ((ptr2 = kvm_server_inputdata((char*)pchRequest2 + ptr, cbBytesRead - ptr)) != 0) { ptr += ptr2; }

		if (KVM_AGENT_FD != -1)
		{
			tmpLen = sprintf_s(tmp, sizeof(tmp), "exited while\n");
			write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);
		}

		if (ptr == len) { len = 0; ptr = 0; }
		// TODO: else move the reminder.
	}

	return 0;
}
void ExitSink(int s)
{
	UNREFERENCED_PARAMETER(s);

	signal(SIGTERM, SIG_IGN);	
	
	if (KVM_Listener_FD > 0) 
	{
		write(STDOUT_FILENO, "EXITING\n", 8);
		fsync(STDOUT_FILENO);
		close(KVM_Listener_FD); 
	}
	g_shutdown = 1;
}
void* kvm_server_mainloop(void* param)
{
	int x, y, height, width, r, c = 0;
	long long desktopsize = 0;
	long long tilesize = 0;
	void *desktop = NULL;
	void *buf = NULL;
	int screen_height, screen_width, screen_num;
	int written = 0;
	struct sockaddr_un serveraddr;

	if (param == NULL)
	{
		// This is doing I/O via StdIn/StdOut
		// Register SIGTERM handler for graceful shutdown (OpenFrame mode)
		// Critical for macOS - without this, SIGKILL corrupts TCC screen recording permission
		signal(SIGTERM, ExitSink);

		int flags;
		flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
		if (fcntl(STDOUT_FILENO, F_SETFL, (O_NONBLOCK | flags) ^ O_NONBLOCK) == -1) {}
	}
	else
	{
		// this is doing I/O via a Unix Domain Socket
		if ((KVM_Listener_FD = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		{
			char tmp[255];
			int tmplen = sprintf_s(tmp, sizeof(tmp), "ERROR CREATING DOMAIN SOCKET: %d\n", errno);
			// Error creating domain socket
			written = write(STDOUT_FILENO, tmp, tmplen);
			fsync(STDOUT_FILENO);
			return(NULL);
		}

		int flags;
		flags = fcntl(KVM_Listener_FD, F_GETFL, 0);
		if (fcntl(KVM_Listener_FD, F_SETFL, (O_NONBLOCK | flags) ^ O_NONBLOCK) == -1) { }

		written = write(STDOUT_FILENO, "Set FCNTL2\n", 11);
		fsync(STDOUT_FILENO);

		memset(&serveraddr, 0, sizeof(serveraddr));
		serveraddr.sun_family = AF_UNIX;
#if USE_LAUNCHAGENT_KVM
		// Use LaunchAgent socket path for daemon<->agent communication
		strcpy(serveraddr.sun_path, KVM_LAUNCHAGENT_SOCKET_PATH);
		remove(KVM_LAUNCHAGENT_SOCKET_PATH);
#else
		strcpy(serveraddr.sun_path, KVM_Listener_Path);
		remove(KVM_Listener_Path);
#endif
		if (bind(KVM_Listener_FD, (struct sockaddr *)&serveraddr, SUN_LEN(&serveraddr)) < 0)
		{
			char tmp[255];
			int tmplen = sprintf_s(tmp, sizeof(tmp), "BIND ERROR on DOMAIN SOCKET: %d\n", errno);
			// Error creating domain socket
			written = write(STDOUT_FILENO, tmp, tmplen);
			fsync(STDOUT_FILENO);
			return(NULL);
		}

		if (listen(KVM_Listener_FD, 1) < 0)
		{
			written = write(STDOUT_FILENO, "LISTEN ERROR ON DOMAIN SOCKET", 29);
			fsync(STDOUT_FILENO);
			return(NULL);
		}

		written = write(STDOUT_FILENO, "LISTENING ON DOMAIN SOCKET\n", 27);
		fsync(STDOUT_FILENO);

		signal(SIGTERM, ExitSink);

		if ((KVM_AGENT_FD = accept(KVM_Listener_FD, NULL, NULL)) < 0)
		{
			written = write(STDOUT_FILENO, "ACCEPT ERROR ON DOMAIN SOCKET", 29);
			fsync(STDOUT_FILENO);
			return(NULL);
		}
		else
		{
			char tmp[255];
			int tmpLen = sprintf_s(tmp, sizeof(tmp), "ACCEPTed new connection %d on Domain Socket\n", KVM_AGENT_FD);
			written = write(STDOUT_FILENO, tmp, tmpLen);
			fsync(STDOUT_FILENO);

		}
	}
	// Init the kvm
	g_messageQ = ILibQueue_Create();
	if (kvm_init() != 0) { return (void*)-1; }


	g_shutdown = 0;
	fprintf(stderr, "[KVM-CHILD] Starting mainloop, g_shutdown=%d, KVM_AGENT_FD=%d\n", g_shutdown, KVM_AGENT_FD);
	fflush(stderr);
	pthread_create(&kvmthread, NULL, kvm_mainloopinput, param);

	if (KVM_AGENT_FD != -1)
	{
		written = write(STDOUT_FILENO, "Starting Loop []\n", 14);
		fsync(STDOUT_FILENO);

		char stmp[255];
		int stmpLen = sprintf_s(stmp, sizeof(stmp), "TILE_HEIGHT_COUNT=%d, TILE_WIDTH_COUNT=%d\n", TILE_HEIGHT_COUNT, TILE_WIDTH_COUNT);
		written = write(STDOUT_FILENO, stmp, stmpLen);
		fsync(STDOUT_FILENO);
	}

	while (!g_shutdown) 
	{
		if (g_resetipc != 0)
		{
			g_resetipc = 0;
			close(KVM_AGENT_FD);

			SCREEN_HEIGHT = SCREEN_WIDTH = 0;

			char stmp[255];
			int stmpLen = sprintf_s(stmp, sizeof(stmp), "Waiting for NEXT DomainSocket, TILE_HEIGHT_COUNT=%d, TILE_WIDTH_COUNT=%d\n", TILE_HEIGHT_COUNT, TILE_WIDTH_COUNT);
			written = write(STDOUT_FILENO, stmp, stmpLen);
			fsync(STDOUT_FILENO);

			if ((KVM_AGENT_FD = accept(KVM_Listener_FD, NULL, NULL)) < 0)
			{
				g_shutdown = 1;
				written = write(STDOUT_FILENO, "ACCEPT ERROR ON DOMAIN SOCKET", 29);
				fsync(STDOUT_FILENO);
				break;
			}
			else
			{
				char tmp[255];
				int tmpLen = sprintf_s(tmp, sizeof(tmp), "ACCEPTed new connection %d on Domain Socket\n", KVM_AGENT_FD);
				written = write(STDOUT_FILENO, tmp, tmpLen);
				fsync(STDOUT_FILENO);
				pthread_create(&kvmthread, NULL, kvm_mainloopinput, param);
			}
		}
		
		// Check if there are pending messages to be sent
		ILibQueue_Lock(g_messageQ);
		while (ILibQueue_IsEmpty(g_messageQ) == 0)
		{
			if ((buf = (char*)ILibQueue_DeQueue(g_messageQ)) != NULL)
			{
				KVM_SEND(buf, (int)ILibMemory_Size(buf));
				ILibMemory_Free(buf);
			}
		}
		ILibQueue_UnLock(g_messageQ);

		for (r = 0; r < TILE_HEIGHT_COUNT; r++) 
		{
			for (c = 0; c < TILE_WIDTH_COUNT; c++) 
			{
				g_tileInfo[r][c].flag = TILE_TODO;
#ifdef KVM_ALL_TILES
				g_tileInfo[r][c].crc = 0xFF;
#endif
			}
		}

		screen_num = CGMainDisplayID();

		if (screen_num == 0) { g_shutdown = 1; senddebug(-2); break; }
		
		if (SCREEN_SCALE_SET == 0)
		{
			CGDisplayModeRef mode = CGDisplayCopyDisplayMode(screen_num);
			if (SCREEN_WIDTH > 0 && SCREEN_SCALE < (int) CGDisplayModeGetPixelWidth(mode) / SCREEN_WIDTH)
			{
				SCREEN_SCALE = (int) CGDisplayModeGetPixelWidth(mode) / SCREEN_WIDTH;
				SCREEN_SCALE_SET = 1;
			}			 
			CGDisplayModeRelease(mode);
		}
		
		screen_height = CGDisplayPixelsHigh(screen_num) * SCREEN_SCALE;
		screen_width = CGDisplayPixelsWide(screen_num) * SCREEN_SCALE;
		
		if ((SCREEN_HEIGHT != screen_height || (SCREEN_WIDTH != screen_width) || SCREEN_NUM != screen_num)) 
		{
			kvm_init();
			continue;
		}

		//senddebug(screen_num);
		static int g_captureCount = 0;
		g_captureCount++;
		if (g_captureCount % 100 == 1) {
			fprintf(stderr, "[KVM-CHILD] Capturing screen #%d, g_shutdown=%d\n", g_captureCount, g_shutdown);
			fflush(stderr);
		}
		CGImageRef image = CGDisplayCreateImage(screen_num);
		//senddebug(99);
		if (image == NULL)
		{
			fprintf(stderr, "[KVM-CHILD] CGDisplayCreateImage returned NULL! screen_num=%d, setting g_shutdown=1\n", screen_num);
			fflush(stderr);
			g_shutdown = 1;
			senddebug(0);
		}
		else {
			//senddebug(100);
			getScreenBuffer((unsigned char **)&desktop, &desktopsize, image);

			if (KVM_AGENT_FD != -1)
			{
				char tmp[255];
				int tmpLen = sprintf_s(tmp, sizeof(tmp), "...Enter for loop\n");
				written = write(STDOUT_FILENO, tmp, tmpLen);
				fsync(STDOUT_FILENO);
			}

			for (y = 0; y < TILE_HEIGHT_COUNT; y++) 
			{
				for (x = 0; x < TILE_WIDTH_COUNT; x++) {
					height = TILE_HEIGHT * y;
					width = TILE_WIDTH * x;
					if (!g_shutdown && (g_pause)) { usleep(100000); g_pause = 0; } //HACK: Change this

					if (g_shutdown) { x = TILE_WIDTH_COUNT; y = TILE_HEIGHT_COUNT; break; }
					
					if (g_tileInfo[y][x].flag == TILE_SENT || g_tileInfo[y][x].flag == TILE_DONT_SEND) {
						continue;
					}
					
					getTileAt(width, height, &buf, &tilesize, desktop, desktopsize, y, x);
					
					if (buf && !g_shutdown)
					{	
						// Write the reply to the pipe.
						//KvmDebugLog("Writing to master in kvm_server_mainloop\n");

						written = KVM_SEND(buf, tilesize);

						//KvmDebugLog("Wrote %d bytes to master in kvm_server_mainloop\n", written);
						if (written == -1)
						{
							fprintf(stderr, "[KVM-CHILD] KVM_SEND failed! errno=%d, KVM_AGENT_FD=%d\n", errno, KVM_AGENT_FD);
							fflush(stderr);
							/*ILIBMESSAGE("KVMBREAK-K2\r\n");*/
							if(KVM_AGENT_FD == -1)
							{
								// This is a User Session, so if the connection fails, we exit out... We can be spawned again later
								fprintf(stderr, "[KVM-CHILD] Setting g_shutdown=1 due to KVM_SEND failure\n");
								fflush(stderr);
								g_shutdown = 1; height = SCREEN_HEIGHT; width = SCREEN_WIDTH; break;
							}
						}
						//else
						//{
						//	char tmp[255];
						//	int tmpLen = sprintf_s(tmp, sizeof(tmp), "KVM_SEND => tilesize: %d\n", tilesize);
						//	written = write(STDOUT_FILENO, tmp, tmpLen);
						//	fsync(STDOUT_FILENO);
						//}
						free(buf);

					}
				}
			}

			if (KVM_AGENT_FD != -1)
			{
				char tmp[255];
				int tmpLen = sprintf_s(tmp, sizeof(tmp), "...exit for loop\n");
				written = write(STDOUT_FILENO, tmp, tmpLen);
				fsync(STDOUT_FILENO);
			}

		}
		CGImageRelease(image);
	}
	
	pthread_join(kvmthread, NULL);
	kvmthread = (pthread_t)NULL;

	if (g_tileInfo != NULL) { for (r = 0; r < TILE_HEIGHT_COUNT; r++) { free(g_tileInfo[r]); } }
	g_tileInfo = NULL;
	if(tilebuffer != NULL) {
		free(tilebuffer);
		tilebuffer = NULL;
	}

	if (KVM_AGENT_FD != -1)
	{
		written = write(STDOUT_FILENO, "Exiting...\n", 11);
		fsync(STDOUT_FILENO);
	}
	ILibQueue_Destroy(g_messageQ);
	return (void*)0;
}

void kvm_relay_ExitHandler(ILibProcessPipe_Process sender, int exitCode, void* user)
{
	//ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)((void**)user)[0];
	//void *reserved = ((void**)user)[1];
	//void *pipeMgr = ((void**)user)[2];
	//char *exePath = (char*)((void**)user)[3];
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(exitCode);
	UNREFERENCED_PARAMETER(user);

	fprintf(stderr, "[KVM] kvm_relay_ExitHandler() called, exitCode=%d, gChildProcess=%p\n", exitCode, (void*)gChildProcess);
	fflush(stderr);

	if (g_openFrameMode)
	{
		// OpenFrame mode: Child process has exited, mark it as NULL to prevent double-free
		gChildProcess = NULL;
		g_slavekvm = 0;
		fprintf(stderr, "[KVM] kvm_relay_ExitHandler() completed, gChildProcess set to NULL (openframe mode)\n");
		fflush(stderr);
	}
	// Standard mode: original behavior - do nothing here
}
static int g_stdoutCallCount = 0;
void kvm_relay_StdOutHandler(ILibProcessPipe_Process sender, char *buffer, size_t bufferLen, size_t* bytesConsumed, void* user)
{
	unsigned short size = 0;
	UNREFERENCED_PARAMETER(sender);
	ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)((void**)user)[0];
	void *reserved = ((void**)user)[1];

	g_stdoutCallCount++;
	// Log every 100th call to avoid spamming
	if (g_stdoutCallCount % 100 == 1)
	{
		fprintf(stderr, "[KVM] StdOutHandler called #%d, bufferLen=%zu, user=%p, writeHandler=%p\n",
			g_stdoutCallCount, bufferLen, user, (void*)writeHandler);
		fflush(stderr);
	}

	if (bufferLen > 4)
	{
		if (ntohs(((unsigned short*)(buffer))[0]) == (unsigned short)MNG_JUMBO)
		{
			if (bufferLen > 8)
			{
				if (bufferLen >= (8 + (int)ntohl(((unsigned int*)(buffer))[1])))
				{
					*bytesConsumed = 8 + (int)ntohl(((unsigned int*)(buffer))[1]);
					TLSLOG1("<< KVM/WRITE: %d bytes\n", *bytesConsumed);
					writeHandler(buffer, *bytesConsumed, reserved);

					//printf("JUMBO PACKET: %d\n", *bytesConsumed);
					return;
				}
			}
		}
		else
		{
			size = ntohs(((unsigned short*)(buffer))[1]);
			if (size <= bufferLen)
			{
				*bytesConsumed = size;
				writeHandler(buffer, size, reserved);
				//printf("Normal PACKET: %d\n", *bytesConsumed);
				return;
			}
		}
	}
	*bytesConsumed = 0;
}
void kvm_relay_StdErrHandler(ILibProcessPipe_Process sender, char *buffer, size_t bufferLen, size_t* bytesConsumed, void* user)
{
	UNREFERENCED_PARAMETER(sender);
	UNREFERENCED_PARAMETER(user);

	// Forward child's stderr to parent's stderr for debugging
	if (bufferLen > 0)
	{
		fwrite(buffer, 1, bufferLen, stderr);
		fflush(stderr);
	}
	*bytesConsumed = bufferLen;
}

#if USE_LAUNCHAGENT_KVM
static int g_socket_read_count = 0;  // Counter for read operations

// Thread function to read data from LaunchAgent socket and forward to writeHandler
void* kvm_socket_read_thread_func(void* arg)
{
	void **user = (void**)arg;
	ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)user[0];
	void *reserved = user[1];

	char buffer[65535];
	ssize_t bytesRead;

	fprintf(stderr, "[KVM-SOCKET] Read thread STARTED, fd=%d, writeHandler=%p, reserved=%p\n",
		g_kvm_socket_fd, (void*)writeHandler, reserved);
	fflush(stderr);

	g_socket_read_count = 0;

	while (g_socket_read_running && g_kvm_socket_fd >= 0)
	{
		bytesRead = read(g_kvm_socket_fd, buffer, sizeof(buffer));
		if (bytesRead > 0)
		{
			g_socket_read_count++;
			if (g_socket_read_count <= 5 || g_socket_read_count % 100 == 0)
			{
				fprintf(stderr, "[KVM-SOCKET] Read #%d: received %zd bytes from agent, fd=%d\n",
					g_socket_read_count, bytesRead, g_kvm_socket_fd);
				fflush(stderr);
			}
			// Forward data to writeHandler (same as kvm_relay_StdOutHandler does)
			if (writeHandler != NULL)
			{
				writeHandler(buffer, (int)bytesRead, reserved);
				if (g_socket_read_count <= 5)
				{
					fprintf(stderr, "[KVM-SOCKET] Read #%d: forwarded %zd bytes to writeHandler\n",
						g_socket_read_count, bytesRead);
					fflush(stderr);
				}
			}
			else
			{
				fprintf(stderr, "[KVM-SOCKET] WARNING: writeHandler is NULL, dropping %zd bytes!\n", bytesRead);
				fflush(stderr);
			}
		}
		else if (bytesRead == 0)
		{
			// Connection closed
			fprintf(stderr, "[KVM-SOCKET] Connection closed by agent (read returned 0), total reads=%d\n", g_socket_read_count);
			fflush(stderr);
			break;
		}
		else
		{
			if (errno == EINTR)
			{
				fprintf(stderr, "[KVM-SOCKET] Read interrupted (EINTR), continuing...\n");
				fflush(stderr);
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				usleep(10000); // 10ms
				continue;
			}
			fprintf(stderr, "[KVM-SOCKET] Read ERROR: errno=%d (%s), fd=%d, total reads=%d\n",
				errno, strerror(errno), g_kvm_socket_fd, g_socket_read_count);
			fflush(stderr);
			break;
		}
	}

	fprintf(stderr, "[KVM-SOCKET] Read thread EXITING, g_socket_read_running=%d, g_kvm_socket_fd=%d, total reads=%d\n",
		g_socket_read_running, g_kvm_socket_fd, g_socket_read_count);
	fflush(stderr);
	return NULL;
}

static int g_socket_send_count = 0;  // Counter for send operations

// Send data to LaunchAgent via socket
int kvm_socket_send(char *buffer, int bufferLen)
{
	if (g_kvm_socket_fd < 0)
	{
		fprintf(stderr, "[KVM-SOCKET] Send FAILED: socket not connected (fd=%d)\n", g_kvm_socket_fd);
		fflush(stderr);
		return -1;
	}

	g_socket_send_count++;
	if (g_socket_send_count <= 5 || g_socket_send_count % 100 == 0)
	{
		fprintf(stderr, "[KVM-SOCKET] Send #%d: sending %d bytes to agent, fd=%d\n",
			g_socket_send_count, bufferLen, g_kvm_socket_fd);
		fflush(stderr);
	}

	ssize_t totalSent = 0;
	while (totalSent < bufferLen)
	{
		ssize_t sent = write(g_kvm_socket_fd, buffer + totalSent, bufferLen - totalSent);
		if (sent < 0)
		{
			if (errno == EINTR) continue;
			fprintf(stderr, "[KVM-SOCKET] Send ERROR: errno=%d (%s), fd=%d, sent %zd of %d bytes\n",
				errno, strerror(errno), g_kvm_socket_fd, totalSent, bufferLen);
			fflush(stderr);
			return -1;
		}
		totalSent += sent;
	}

	if (g_socket_send_count <= 5)
	{
		fprintf(stderr, "[KVM-SOCKET] Send #%d: successfully sent %d bytes\n", g_socket_send_count, bufferLen);
		fflush(stderr);
	}
	return (int)totalSent;
}
#endif

// Setup the KVM session. Return 1 if ok, 0 if it could not be setup.
void* kvm_relay_setup(char *exePath, void *processPipeMgr, ILibKVM_WriteHandler writeHandler, void *reserved, int uid, int openFrameMode)
{
	// Store openFrameMode for use in kvm_cleanup()
	g_openFrameMode = openFrameMode;

	char *binaryName;
	if (g_openFrameMode)
	{
		// OpenFrame mode: Extract binary name from exePath for argv[0] (needed for macOS TCC permission matching)
		binaryName = strrchr(exePath, '/');
		binaryName = (binaryName != NULL) ? binaryName + 1 : exePath;
	}
	else
	{
		// Standard mode: use hardcoded name
		binaryName = "meshagent_osx64";
	}

	char * parms0[] = { binaryName, "-kvm0", NULL };
	void **user = (void**)ILibMemory_Allocate(4 * sizeof(void*), 0, NULL, NULL);
	user[0] = writeHandler;
	user[1] = reserved;
	user[2] = processPipeMgr;
	user[3] = exePath;

	if (uid != 0)
	{
#if USE_LAUNCHAGENT_KVM
		fprintf(stderr, "[KVM-SETUP] ENTER kvm_relay_setup() uid=%d, openFrameMode=%d, USE_LAUNCHAGENT_KVM=1\n", uid, g_openFrameMode);
		fprintf(stderr, "[KVM-SETUP] exePath=%s, writeHandler=%p, reserved=%p\n", exePath, (void*)writeHandler, reserved);
		fprintf(stderr, "[KVM-SETUP] Current state: g_kvm_socket_fd=%d, g_shutdown=%d, g_cleanup_in_progress=%d\n",
			g_kvm_socket_fd, g_shutdown, g_cleanup_in_progress);
		fflush(stderr);

		if (g_openFrameMode)
		{
			// LaunchAgent mode: connect to existing LaunchAgent socket instead of spawning child
			fprintf(stderr, "[KVM-SETUP] LaunchAgent mode ACTIVE, will try to connect to socket\n");
			fflush(stderr);

			g_shutdown = 0;
			g_cleanup_in_progress = 0;
			g_socket_send_count = 0;  // Reset send counter for new session

			// Check if already connected
			if (g_kvm_socket_fd >= 0)
			{
				fprintf(stderr, "[KVM-SETUP] REUSING existing socket connection, fd=%d\n", g_kvm_socket_fd);
				fflush(stderr);

				// Send unpause command
				fprintf(stderr, "[KVM-SETUP] Sending unpause command to agent...\n");
				fflush(stderr);
				char pauseBuffer[5];
				((unsigned short*)pauseBuffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_PAUSE);
				((unsigned short*)pauseBuffer)[1] = (unsigned short)htons((unsigned short)5);
				pauseBuffer[4] = 0;  // unpause
				kvm_relay_feeddata(pauseBuffer, 5);

				// Send refresh command
				fprintf(stderr, "[KVM-SETUP] Sending refresh command to agent...\n");
				fflush(stderr);
				kvm_relay_reset();

				g_kvm_socket_user = user;
				fprintf(stderr, "[KVM-SETUP] Socket reuse complete, returning fd=%d\n", g_kvm_socket_fd);
				fflush(stderr);
				return (void*)(intptr_t)g_kvm_socket_fd;
			}

			// Try to connect to LaunchAgent socket
			fprintf(stderr, "[KVM-SETUP] Creating new socket for LaunchAgent connection...\n");
			fflush(stderr);

			struct sockaddr_un serveraddr;
			int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
			if (sock_fd < 0)
			{
				fprintf(stderr, "[KVM-SETUP] FAILED to create socket: errno=%d (%s)\n", errno, strerror(errno));
				fflush(stderr);
				return NULL;
			}
			fprintf(stderr, "[KVM-SETUP] Created socket fd=%d\n", sock_fd);
			fflush(stderr);

			memset(&serveraddr, 0, sizeof(serveraddr));
			serveraddr.sun_family = AF_UNIX;
			strcpy(serveraddr.sun_path, KVM_LAUNCHAGENT_SOCKET_PATH);

			fprintf(stderr, "[KVM-SETUP] Connecting to LaunchAgent socket: %s\n", KVM_LAUNCHAGENT_SOCKET_PATH);
			fflush(stderr);

			// Check if socket file exists
			struct stat st;
			if (stat(KVM_LAUNCHAGENT_SOCKET_PATH, &st) != 0)
			{
				fprintf(stderr, "[KVM-SETUP] WARNING: Socket file does not exist! errno=%d (%s)\n", errno, strerror(errno));
				fprintf(stderr, "[KVM-SETUP] Is LaunchAgent running? Check: launchctl list | grep mesh\n");
				fflush(stderr);
			}
			else
			{
				fprintf(stderr, "[KVM-SETUP] Socket file exists, mode=%o, uid=%d, gid=%d\n",
					st.st_mode, st.st_uid, st.st_gid);
				fflush(stderr);
			}

			if (connect(sock_fd, (struct sockaddr *)&serveraddr, SUN_LEN(&serveraddr)) < 0)
			{
				fprintf(stderr, "[KVM-SETUP] FAILED to connect to LaunchAgent socket: errno=%d (%s)\n", errno, strerror(errno));
				fprintf(stderr, "[KVM-SETUP] Possible causes:\n");
				fprintf(stderr, "[KVM-SETUP]   1. LaunchAgent is not running\n");
				fprintf(stderr, "[KVM-SETUP]   2. LaunchAgent has not created the socket yet\n");
				fprintf(stderr, "[KVM-SETUP]   3. Permission denied to connect\n");
				fflush(stderr);
				close(sock_fd);
				return NULL;
			}

			fprintf(stderr, "[KVM-SETUP] SUCCESS! Connected to LaunchAgent socket, fd=%d\n", sock_fd);
			fflush(stderr);

			g_kvm_socket_fd = sock_fd;
			g_kvm_socket_user = user;

			// Start read thread to receive data from agent
			fprintf(stderr, "[KVM-SETUP] Creating read thread for socket communication...\n");
			fflush(stderr);
			g_socket_read_running = 1;
			if (pthread_create(&g_socket_read_thread, NULL, kvm_socket_read_thread_func, user) != 0)
			{
				fprintf(stderr, "[KVM-SETUP] FAILED to create socket read thread: errno=%d (%s)\n", errno, strerror(errno));
				fflush(stderr);
				close(g_kvm_socket_fd);
				g_kvm_socket_fd = -1;
				return NULL;
			}

			fprintf(stderr, "[KVM-SETUP] Read thread created successfully, returning fd=%d\n", g_kvm_socket_fd);
			fflush(stderr);
			return (void*)(intptr_t)g_kvm_socket_fd;
		}
#else
		if (g_openFrameMode)
		{
			// OpenFrame mode: Reset state BEFORE spawning child so it inherits clean state
			g_shutdown = 0;
			g_cleanup_in_progress = 0;

			// OpenFrame mode: Check if we already have a living child process - reuse it!
			// This is critical for macOS screen recording - killing child corrupts TCC permission state
			if (gChildProcess != NULL && g_slavekvm > 0)
			{
				// Check if child is still alive
				if (kill(g_slavekvm, 0) == 0)
				{
					fprintf(stderr, "[KVM] kvm_relay_setup() reusing existing child process, pid=%d\n", g_slavekvm);
					fflush(stderr);

					// Update user object for the new session
					ILibProcessPipe_Process_UpdateUserObject(gChildProcess, user);

					// Resume the stdout pipe in case it was paused
					ILibProcessPipe_Pipe stdoutPipe = ILibProcessPipe_Process_GetStdOut(gChildProcess);
					if (stdoutPipe != NULL)
					{
						ILibProcessPipe_Pipe_Resume(stdoutPipe);
						fprintf(stderr, "[KVM] kvm_relay_setup() resumed stdout pipe\n");
						fflush(stderr);
					}

					// Send unpause command first (in case child was paused during cleanup)
					char pauseBuffer[5];
					((unsigned short*)pauseBuffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_PAUSE);
					((unsigned short*)pauseBuffer)[1] = (unsigned short)htons((unsigned short)5);
					pauseBuffer[4] = 0;  // unpause
					kvm_relay_feeddata(pauseBuffer, 5);

					// Send refresh command to restart screen capture
					kvm_relay_reset();

					fprintf(stderr, "[KVM] kvm_relay_setup() sent unpause and refresh commands to child\n");
					fflush(stderr);
					g_stdoutCallCount = 0; // Reset counter for reused session

					return(stdoutPipe);
				}
				else
				{
					fprintf(stderr, "[KVM] kvm_relay_setup() old child process %d is dead, spawning new\n", g_slavekvm);
					fflush(stderr);
					gChildProcess = NULL;
					g_slavekvm = 0;
				}
			}
		}
#endif
		else
		{
			// Standard mode: original behavior - just reset g_shutdown
			g_shutdown = 0;
		}

		// Spawn child kvm process into a specific user session
		gChildProcess = ILibProcessPipe_Manager_SpawnProcessEx3(processPipeMgr, exePath, parms0, ILibProcessPipe_SpawnTypes_DEFAULT, (void*)(uint64_t)uid, 0);
		g_slavekvm = ILibProcessPipe_Process_GetPID(gChildProcess);

		char tmp[255];
		sprintf_s(tmp, sizeof(tmp), "Child KVM (pid: %d)", g_slavekvm);
		ILibProcessPipe_Process_ResetMetadata(gChildProcess, tmp);

		ILibProcessPipe_Process_AddHandlers(gChildProcess, 65535, &kvm_relay_ExitHandler, &kvm_relay_StdOutHandler, &kvm_relay_StdErrHandler, NULL, user);

		fprintf(stderr, "[KVM] kvm_relay_setup() started new session, pid=%d, binaryName=%s, exePath=%s, openFrameMode=%d\n", g_slavekvm, binaryName, exePath, g_openFrameMode);
		g_stdoutCallCount = 0; // Reset counter for new session
		fflush(stderr);
		return(ILibProcessPipe_Process_GetStdOut(gChildProcess));
	}
	else
	{
		// No users are logged in. This is a special case for MacOS
		//int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		//if (!fd < 0)
		//{
		//	struct sockaddr_un serveraddr;
		//	memset(&serveraddr, 0, sizeof(serveraddr));
		//	serveraddr.sun_family = AF_UNIX;
		//	strcpy(serveraddr.sun_path, KVM_Listener_Path);
		//	if (!connect(fd, (struct sockaddr *)&serveraddr, SUN_LEN(&serveraddr)) < 0)
		//	{
		//		return((void*)(uint64_t)fd);
		//	}
		//}
		return((void*)KVM_Listener_Path);
	}
}

// Force a KVM reset & refresh
void kvm_relay_reset()
{
	char buffer[4];
	((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_REFRESH);	// Write the type
	((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)4);				// Write the size
	kvm_relay_feeddata(buffer, 4);
}

// Clean up the KVM session.
void kvm_cleanup()
{
	fprintf(stderr, "[KVM] kvm_cleanup() called, g_cleanup_in_progress=%d, g_shutdown=%d, gChildProcess=%p, g_slavekvm=%d, g_openFrameMode=%d, g_kvm_socket_fd=%d\n",
		g_cleanup_in_progress, g_shutdown, (void*)gChildProcess, g_slavekvm, g_openFrameMode, g_kvm_socket_fd);
	fflush(stderr);

	if (g_openFrameMode)
	{
		// OpenFrame mode: guard against double cleanup and keep child alive for reuse
		if (g_cleanup_in_progress)
		{
			fprintf(stderr, "[KVM] kvm_cleanup() already in progress, skipping\n");
			fflush(stderr);
			return;
		}
		g_cleanup_in_progress = 1;

		g_shutdown = 1;

#if USE_LAUNCHAGENT_KVM
		// LaunchAgent socket mode: send pause and keep socket open for reuse
		if (g_kvm_socket_fd >= 0)
		{
			fprintf(stderr, "[KVM] kvm_cleanup() sending pause command via socket (LaunchAgent mode)\n");
			fflush(stderr);
			// Send pause command to reduce CPU usage while idle
			char buffer[5];
			((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_PAUSE);
			((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)5);
			buffer[4] = 1;  // pause = 1
			kvm_socket_send(buffer, 5);

			// Keep the socket open for reuse on next connection
			// The LaunchAgent process maintains its TCC permissions
			fprintf(stderr, "[KVM] kvm_cleanup() keeping socket open for reuse\n");
			fflush(stderr);
		}
		else
		{
			fprintf(stderr, "[KVM] kvm_cleanup() no socket to clean up\n");
			fflush(stderr);
		}
#else
		if (gChildProcess != NULL && g_slavekvm > 0)
		{
			// OpenFrame mode: keep the child process alive for reuse
			// Killing the child process corrupts macOS TCC screen recording permission state
			// The child will be reused on next kvm_relay_setup() call
			if (kill(g_slavekvm, 0) == 0)
			{
				fprintf(stderr, "[KVM] kvm_cleanup() keeping child process %d alive for reuse (openframe mode)\n", g_slavekvm);
				fflush(stderr);
				// Send pause command to reduce CPU usage while idle
				char buffer[5];
				((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_PAUSE);
				((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)5);
				buffer[4] = 1;  // pause = 1
				kvm_relay_feeddata(buffer, 5);
				fprintf(stderr, "[KVM] kvm_cleanup() sent pause command to child\n");
				fflush(stderr);
			}
			else
			{
				fprintf(stderr, "[KVM] kvm_cleanup() child process %d already dead\n", g_slavekvm);
				fflush(stderr);
				gChildProcess = NULL;
				g_slavekvm = 0;
			}
		}
		else
		{
			fprintf(stderr, "[KVM] kvm_cleanup() no child process to clean up\n");
			fflush(stderr);
		}
#endif

		// Reset cleanup guard so next kvm_relay_setup can work
		g_cleanup_in_progress = 0;
	}
	else
	{
		// Standard mode: original behavior - kill the child process
		KvmDebugLog("kvm_cleanup\n");
		g_shutdown = 1;
		if (gChildProcess != NULL)
		{
			fprintf(stderr, "[KVM] kvm_cleanup() killing child process (standard mode)\n");
			fflush(stderr);
			ILibProcessPipe_Process_SoftKill(gChildProcess);
			gChildProcess = NULL;
		}
	}

	fprintf(stderr, "[KVM] kvm_cleanup() completed\n");
	fflush(stderr);
}


typedef enum {
    MPAuthorizationStatusNotDetermined,
    MPAuthorizationStatusAuthorized,
    MPAuthorizationStatusDenied
} MPAuthorizationStatus;




MPAuthorizationStatus _checkFDAUsingFile(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd != -1)
    {
        close(fd);
        return MPAuthorizationStatusAuthorized;
    }

    if (errno == EPERM || errno == EACCES)
    {
        return MPAuthorizationStatusDenied;
    }

    return MPAuthorizationStatusNotDetermined;
}

MPAuthorizationStatus _fullDiskAuthorizationStatus() {
    char *userHomeFolderPath = getenv("HOME");
    if (userHomeFolderPath == NULL) {
        struct passwd *pw = getpwuid(getuid());
        if (pw == NULL) {
            return MPAuthorizationStatusNotDetermined;
        }
        userHomeFolderPath = pw->pw_dir;
    }

    const char *testFiles[] = {
        strcat(strcpy(malloc(strlen(userHomeFolderPath) + 30), userHomeFolderPath), "/Library/Safari/CloudTabs.db"),
        strcat(strcpy(malloc(strlen(userHomeFolderPath) + 30), userHomeFolderPath), "/Library/Safari/Bookmarks.plist"),
        "/Library/Application Support/com.apple.TCC/TCC.db",
        "/Library/Preferences/com.apple.TimeMachine.plist",
    };

    MPAuthorizationStatus resultStatus = MPAuthorizationStatusNotDetermined;
    for (int i = 0; i < 4; i++) {
        MPAuthorizationStatus status = _checkFDAUsingFile(testFiles[i]);
        if (status == MPAuthorizationStatusAuthorized) {
            resultStatus = MPAuthorizationStatusAuthorized;
            break;
        }
        if (status == MPAuthorizationStatusDenied) {
            resultStatus = MPAuthorizationStatusDenied;
        }
    }

    return resultStatus;
}


void kvm_check_permission()
{

    //Request screen recording access
    if(__builtin_available(macOS 10.15, *)){
        if(!CGPreflightScreenCaptureAccess()) {
            CGRequestScreenCaptureAccess();
        }
    }


    // Request accessibility access
    if(__builtin_available(macOS 10.9, *)){
        const void * keys[] = { kAXTrustedCheckOptionPrompt };
        const void * values[] = { kCFBooleanTrue };

        CFDictionaryRef options = CFDictionaryCreate(
            kCFAllocatorDefault,
            keys,
            values,
            sizeof(keys) / sizeof(*keys),
            &kCFCopyStringDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);

        AXIsProcessTrustedWithOptions(options);
    }

    // Request full disk access
    if(__builtin_available(macOS 10.14, *)) {
        if(_fullDiskAuthorizationStatus() != MPAuthorizationStatusAuthorized) {
            CFStringRef URL =  CFStringCreateWithCString(NULL, "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles", kCFStringEncodingASCII);
            CFURLRef pathRef = CFURLCreateWithString( NULL, URL, NULL );
            if( pathRef )
            {
                LSOpenCFURLRef(pathRef, NULL);
                CFRelease(pathRef);
            }
            CFRelease(URL);
        }
    }
}