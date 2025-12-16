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
#include <sys/stat.h>

#include <string.h>
#include <pwd.h>
#include <bsm/libbsm.h>
#include <errno.h>

int KVM_Listener_FD = -1;
int g_kvm_socket_fd = -1;      // Socket fd when using launchctl asuser mode
int g_kvm_listen_fd = -1;      // Listening socket fd for launchctl asuser mode
void *g_kvm_socket_user = NULL; // User data for socket mode
char g_kvm_socket_path[256] = {0}; // Socket path for launchctl asuser mode
#define KVM_Listener_Path "/usr/local/mesh_services/meshagent/kvm"
#define USE_LAUNCHCTL_ASUSER 1  // Enable launchctl asuser mode for OpenFrame
#define KVM_LAUNCHCTL_SOCKET_PATH "/tmp/meshkvm_daemon.sock"  // Fixed socket path for launchctl asuser mode
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
int g_launchctlSocketMode = 0;  // Flag: child connected via launchctl asuser socket (should handle input events)
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
			// Allow key events if: size is correct AND (not using IPC or in launchctl socket mode)
			if (size != 6 || (KVM_AGENT_FD != -1 && !g_launchctlSocketMode)) {
				static int key_skip_count = 0;
				if (++key_skip_count <= 5)
				{
					fprintf(stderr, "[KVM-INPUT] KEY event SKIPPED #%d: size=%d, KVM_AGENT_FD=%d, g_launchctlSocketMode=%d\n",
						key_skip_count, size, KVM_AGENT_FD, g_launchctlSocketMode);
					fflush(stderr);
				}
				break;
			}
			fprintf(stderr, "[KVM-INPUT] KEY event: key=%d, action=%d\n", block[5], block[4]);
			fflush(stderr);
			KeyAction(block[5], block[4]);
			break;
		}
		case MNG_KVM_MOUSE: // Mouse
		{
			int x, y;
			short w = 0;
			// Allow mouse events if: not using IPC or in launchctl socket mode
			if (KVM_AGENT_FD != -1 && !g_launchctlSocketMode) {
				static int mouse_skip_count = 0;
				if (++mouse_skip_count <= 5)
				{
					fprintf(stderr, "[KVM-INPUT] MOUSE event SKIPPED #%d: KVM_AGENT_FD=%d, g_launchctlSocketMode=%d\n",
						mouse_skip_count, KVM_AGENT_FD, g_launchctlSocketMode);
					fflush(stderr);
				}
				break;
			}
			if (size == 10 || size == 12)
			{
				x = ((int)ntohs(((unsigned short*)(block))[3])) / SCREEN_SCALE;
				y = ((int)ntohs(((unsigned short*)(block))[4])) / SCREEN_SCALE;

				if (size == 12) w = ((short)ntohs(((short*)(block))[5]));

				static int mouse_event_count = 0;
				if (++mouse_event_count <= 5)
				{
					fprintf(stderr, "[KVM-INPUT] MOUSE event #%d: x=%d, y=%d, btn=%d, wheel=%d\n",
						mouse_event_count, x, y, (int)(unsigned char)(block[5]), w);
					fflush(stderr);
				}
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
			fprintf(stderr, "[KVM-CHILD] REFRESH processed, tiles reset, g_shutdown=%d, g_remotepause=%d\n",
				g_shutdown, g_remotepause);
			fflush(stderr);
			break;
		}
		case MNG_KVM_PAUSE: // Pause
		{
			if (size != 5) break;
			fprintf(stderr, "[KVM-CHILD] PAUSE received, value=%d, g_shutdown=%d, g_remotepause(before)=%d\n",
				block[4], g_shutdown, g_remotepause);
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


int kvm_relay_feeddata(char* buf, int len)
{
	// If using socket mode (launchctl asuser), write to socket
	if (g_kvm_socket_fd >= 0)
	{
		int written = write(g_kvm_socket_fd, buf, len);
		if (written < 0)
		{
			fprintf(stderr, "[KVM] kvm_relay_feeddata socket write error: %d\n", errno);
			fflush(stderr);
		}
		return written;
	}

	// Otherwise use pipe to child process
	if (gChildProcess != NULL)
	{
		ILibProcessPipe_Process_WriteStdIn(gChildProcess, buf, len, ILibTransport_MemoryOwnership_USER);
	}
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
		// Register SIGTERM handler for graceful shutdown (OpenFrame mode)
		// Critical for macOS - without this, SIGKILL corrupts TCC screen recording permission
		signal(SIGTERM, ExitSink);

#if USE_LAUNCHCTL_ASUSER
		// Check if daemon's socket exists - if so, we were launched via launchctl asuser
		// and should connect to daemon's socket instead of using stdin/stdout
		struct stat socket_stat;
		if (stat(KVM_LAUNCHCTL_SOCKET_PATH, &socket_stat) == 0 && (socket_stat.st_mode & S_IFSOCK))
		{
			fprintf(stderr, "[KVM-CHILD] Detected daemon socket at %s - connecting via launchctl asuser mode\n",
				KVM_LAUNCHCTL_SOCKET_PATH);
			fflush(stderr);

			// Connect to daemon's socket
			int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
			if (sock_fd >= 0)
			{
				struct sockaddr_un sock_addr;
				memset(&sock_addr, 0, sizeof(sock_addr));
				sock_addr.sun_family = AF_UNIX;
				strncpy(sock_addr.sun_path, KVM_LAUNCHCTL_SOCKET_PATH, sizeof(sock_addr.sun_path) - 1);

				// Retry connection
				int connect_retries = 10;
				while (connect_retries > 0)
				{
					fprintf(stderr, "[KVM-CHILD] Attempting connect to daemon socket (try %d/10)...\n",
						11 - connect_retries);
					fflush(stderr);

					if (connect(sock_fd, (struct sockaddr *)&sock_addr, SUN_LEN(&sock_addr)) == 0)
					{
						fprintf(stderr, "[KVM-CHILD] Connected to daemon socket! fd=%d\n", sock_fd);
						fflush(stderr);

						// Use socket for I/O instead of stdin/stdout
						KVM_AGENT_FD = sock_fd;
						g_launchctlSocketMode = 1; // Enable input event handling in child

						// Test TCC permission immediately
						fprintf(stderr, "[KVM-CHILD] === Testing TCC Screen Recording permission ===\n");
						bool preflight = CGPreflightScreenCaptureAccess();
						fprintf(stderr, "[KVM-CHILD] CGPreflightScreenCaptureAccess() = %s\n",
							preflight ? "true" : "false");

						CGDirectDisplayID mainDisplay = CGMainDisplayID();
						fprintf(stderr, "[KVM-CHILD] CGMainDisplayID() = %u\n", mainDisplay);

						CGImageRef testImage = CGDisplayCreateImage(mainDisplay);
						if (testImage != NULL)
						{
							size_t w = CGImageGetWidth(testImage);
							size_t h = CGImageGetHeight(testImage);
							fprintf(stderr, "[KVM-CHILD] TEST CAPTURE SUCCESS! size=%zux%zu\n", w, h);
							CGImageRelease(testImage);
						}
						else
						{
							fprintf(stderr, "[KVM-CHILD] TEST CAPTURE FAILED! CGDisplayCreateImage returned NULL\n");
						}
						fprintf(stderr, "[KVM-CHILD] === End TCC test ===\n");
						fflush(stderr);

						// Continue to mainloop with socket I/O
						break;
					}

					fprintf(stderr, "[KVM-CHILD] connect() failed: errno=%d (%s)\n", errno, strerror(errno));
					fflush(stderr);
					usleep(100000); // 100ms
					connect_retries--;
				}

				if (connect_retries == 0)
				{
					fprintf(stderr, "[KVM-CHILD] Failed to connect to daemon socket after 10 retries\n");
					fflush(stderr);
					close(sock_fd);
					// Fall back to stdin/stdout mode
				}
			}
			else
			{
				fprintf(stderr, "[KVM-CHILD] Failed to create socket: errno=%d\n", errno);
				fflush(stderr);
			}
		}
		else
		{
			fprintf(stderr, "[KVM-CHILD] No daemon socket found at %s - using stdin/stdout mode\n",
				KVM_LAUNCHCTL_SOCKET_PATH);
			fflush(stderr);
		}
#endif

		// Standard stdin/stdout mode (if KVM_AGENT_FD was not set above)
		if (KVM_AGENT_FD == -1)
		{
			int flags;
			flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
			if (fcntl(STDOUT_FILENO, F_SETFL, (O_NONBLOCK | flags) ^ O_NONBLOCK) == -1) {}
		}
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
		strcpy(serveraddr.sun_path, KVM_Listener_Path);
		remove(KVM_Listener_Path);
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

	// Extended diagnostics at child startup
	fprintf(stderr, "[KVM-CHILD-DIAG] === CHILD PROCESS DIAGNOSTICS ===\n");
	fprintf(stderr, "[KVM-CHILD-DIAG] PID=%d, PPID=%d, UID=%d, EUID=%d, GID=%d, EGID=%d\n",
		getpid(), getppid(), getuid(), geteuid(), getgid(), getegid());
	fflush(stderr);

	// Check audit session
	{
		auditinfo_addr_t ainfo;
		memset(&ainfo, 0, sizeof(ainfo));
		if (getaudit_addr(&ainfo, sizeof(ainfo)) == 0) {
			fprintf(stderr, "[KVM-CHILD-DIAG] Audit: ASID=%d, AUID=%d, EUID=%d, EGID=%d, termid.port=%d\n",
				ainfo.ai_asid, ainfo.ai_auid, ainfo.ai_mask.am_success, ainfo.ai_mask.am_failure, ainfo.ai_termid.at_port);
		} else {
			fprintf(stderr, "[KVM-CHILD-DIAG] Audit: getaudit_addr failed, errno=%d\n", errno);
		}
		fflush(stderr);
	}

	// Check main display
	{
		CGDirectDisplayID mainDisplay = CGMainDisplayID();
		fprintf(stderr, "[KVM-CHILD-DIAG] CGMainDisplayID()=%u\n", mainDisplay);

		uint32_t displayCount = 0;
		CGDirectDisplayID displays[16];
		CGGetActiveDisplayList(16, displays, &displayCount);
		fprintf(stderr, "[KVM-CHILD-DIAG] Active displays count=%u\n", displayCount);
		for (uint32_t i = 0; i < displayCount && i < 16; i++) {
			fprintf(stderr, "[KVM-CHILD-DIAG]   Display[%u]: ID=%u, Width=%zu, Height=%zu\n",
				i, displays[i], CGDisplayPixelsWide(displays[i]), CGDisplayPixelsHigh(displays[i]));
		}
		fflush(stderr);
	}

	// Check TCC permission status multiple ways
	if (__builtin_available(macOS 10.15, *)) {
		bool preflight = CGPreflightScreenCaptureAccess();
		fprintf(stderr, "[KVM-CHILD-DIAG] CGPreflightScreenCaptureAccess() at startup = %s\n", preflight ? "true" : "false");
		fflush(stderr);
	}

	// Try a test capture immediately
	{
		CGDirectDisplayID testDisplay = CGMainDisplayID();
		fprintf(stderr, "[KVM-CHILD-DIAG] Test capture: display=%u\n", testDisplay);
		fflush(stderr);

		CGImageRef testImage = CGDisplayCreateImage(testDisplay);
		if (testImage != NULL) {
			size_t w = CGImageGetWidth(testImage);
			size_t h = CGImageGetHeight(testImage);
			fprintf(stderr, "[KVM-CHILD-DIAG] Test capture SUCCESS! image=%p, size=%zux%zu\n", (void*)testImage, w, h);
			CGImageRelease(testImage);
		} else {
			fprintf(stderr, "[KVM-CHILD-DIAG] Test capture FAILED! CGDisplayCreateImage returned NULL\n");
		}
		fflush(stderr);
	}

	fprintf(stderr, "[KVM-CHILD-DIAG] === END DIAGNOSTICS ===\n");
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

		// Detailed diagnostics for debugging capture failures
		static int g_diagCount = 0;
		g_diagCount++;
		if (g_diagCount <= 5 || g_diagCount % 50 == 0) {
			fprintf(stderr, "[KVM-DIAG] Loop #%d: screen_num=%u, g_shutdown=%d, g_remotepause=%d\n",
				g_diagCount, screen_num, g_shutdown, g_remotepause);
			fflush(stderr);
		}

		if (screen_num == 0) {
			fprintf(stderr, "[KVM-DIAG] CGMainDisplayID returned 0! Setting g_shutdown=1\n");
			fflush(stderr);
			g_shutdown = 1; senddebug(-2); break;
		}
		
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
		static int g_nullRetryCount = 0;  // Retry counter for CGDisplayCreateImage failures
		static int g_permissionChecked = 0;  // Flag to check permission once

		// Log static variable states for debugging
		if (g_diagCount <= 5 || g_nullRetryCount > 0) {
			fprintf(stderr, "[KVM-DIAG] Static vars: g_nullRetryCount=%d, g_permissionChecked=%d, g_captureCount=%d\n",
				g_nullRetryCount, g_permissionChecked, g_captureCount);
			fflush(stderr);
		}

		// Check TCC permission status before capture attempt
		if (g_nullRetryCount == 0 && g_permissionChecked == 0)
		{
			g_permissionChecked = 1;
			if (__builtin_available(macOS 10.15, *))
			{
				bool hasAccess = CGPreflightScreenCaptureAccess();
				fprintf(stderr, "[KVM-CHILD] CGPreflightScreenCaptureAccess() = %s\n", hasAccess ? "true" : "false");
				fflush(stderr);

				if (!hasAccess)
				{
					fprintf(stderr, "[KVM-CHILD] No screen recording permission, requesting...\n");
					fflush(stderr);
					bool requested = CGRequestScreenCaptureAccess();
					fprintf(stderr, "[KVM-CHILD] CGRequestScreenCaptureAccess() = %s\n", requested ? "true" : "false");
					fflush(stderr);
				}
			}
		}

		// Log before CGDisplayCreateImage call
		if (g_diagCount <= 5 || g_nullRetryCount > 0) {
			fprintf(stderr, "[KVM-DIAG] About to call CGDisplayCreateImage(screen_num=%u)\n", screen_num);
			fflush(stderr);
		}

		CGImageRef image = CGDisplayCreateImage(screen_num);

		// Log result of CGDisplayCreateImage
		if (g_diagCount <= 5 || image == NULL) {
			fprintf(stderr, "[KVM-DIAG] CGDisplayCreateImage result: image=%p (NULL=%s)\n",
				(void*)image, image == NULL ? "YES" : "NO");
			fflush(stderr);
		}

		//senddebug(99);
		if (image == NULL)
		{
			g_nullRetryCount++;

			// Extended diagnostics on first NULL
			if (g_nullRetryCount == 1) {
				fprintf(stderr, "[KVM-DIAG-NULL] === FIRST NULL DIAGNOSTICS ===\n");

				// Re-check permission status
				if (__builtin_available(macOS 10.15, *)) {
					bool preflight2 = CGPreflightScreenCaptureAccess();
					fprintf(stderr, "[KVM-DIAG-NULL] CGPreflightScreenCaptureAccess() after NULL = %s\n", preflight2 ? "true" : "false");

					// Try requesting again
					fprintf(stderr, "[KVM-DIAG-NULL] Calling CGRequestScreenCaptureAccess()...\n");
					bool request2 = CGRequestScreenCaptureAccess();
					fprintf(stderr, "[KVM-DIAG-NULL] CGRequestScreenCaptureAccess() = %s\n", request2 ? "true" : "false");

					// Check again after request
					bool preflight3 = CGPreflightScreenCaptureAccess();
					fprintf(stderr, "[KVM-DIAG-NULL] CGPreflightScreenCaptureAccess() after request = %s\n", preflight3 ? "true" : "false");
				}

				// Check process info again
				fprintf(stderr, "[KVM-DIAG-NULL] PID=%d, PPID=%d, UID=%d, EUID=%d\n",
					getpid(), getppid(), getuid(), geteuid());

				// Check audit session again
				auditinfo_addr_t ainfo2;
				memset(&ainfo2, 0, sizeof(ainfo2));
				if (getaudit_addr(&ainfo2, sizeof(ainfo2)) == 0) {
					fprintf(stderr, "[KVM-DIAG-NULL] Current Audit: ASID=%d, AUID=%d\n",
						ainfo2.ai_asid, ainfo2.ai_auid);
				}

				// Check display info
				CGDirectDisplayID mainDisp = CGMainDisplayID();
				fprintf(stderr, "[KVM-DIAG-NULL] CGMainDisplayID()=%u, requested screen_num=%d\n", mainDisp, screen_num);

				// Check if display is asleep or has changed
				boolean_t isAsleep = CGDisplayIsAsleep(screen_num);
				boolean_t isOnline = CGDisplayIsOnline(screen_num);
				boolean_t isMain = CGDisplayIsMain(screen_num);
				fprintf(stderr, "[KVM-DIAG-NULL] Display state: isAsleep=%d, isOnline=%d, isMain=%d\n",
					isAsleep, isOnline, isMain);

				fprintf(stderr, "[KVM-DIAG-NULL] === END FIRST NULL DIAGNOSTICS ===\n");
				fflush(stderr);
			}

			// Retry up to 30 times (30 seconds) - GUI may not be ready after reboot
			if (g_nullRetryCount <= 30)
			{
				fprintf(stderr, "[KVM-CHILD] CGDisplayCreateImage returned NULL! screen_num=%d, retry %d/30, waiting 1 second...\n", screen_num, g_nullRetryCount);
				fflush(stderr);
				sleep(1);
				continue;  // Retry the capture
			}

			fprintf(stderr, "[KVM-CHILD] CGDisplayCreateImage returned NULL after 30 retries! screen_num=%d, setting g_shutdown=1\n", screen_num);
			fflush(stderr);
			g_shutdown = 1;
			senddebug(0);
		}
		else {
			// Reset retry counter on successful capture
			g_nullRetryCount = 0;

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

#if USE_LAUNCHCTL_ASUSER
// Socket read thread for launchctl asuser mode
static pthread_t g_socket_read_thread = (pthread_t)NULL;
static int g_socket_read_running = 0;

static void* kvm_socket_read_thread(void* arg)
{
	void **user = (void**)arg;
	ILibKVM_WriteHandler writeHandler = (ILibKVM_WriteHandler)user[0];
	void *reserved = user[1];

	// Use dynamic buffer - start with 512KB, can handle most JUMBO packets
	int buffer_size = 524288; // 512KB
	char *buffer = (char*)malloc(buffer_size);
	if (buffer == NULL)
	{
		fprintf(stderr, "[KVM-SOCKET-THREAD] Failed to allocate buffer!\n");
		fflush(stderr);
		return NULL;
	}

	int offset = 0;
	int read_count = 0;
	int message_count = 0;

	fprintf(stderr, "[KVM-SOCKET-THREAD] Read thread started, fd=%d, writeHandler=%p, reserved=%p, buffer_size=%d\n",
		g_kvm_socket_fd, (void*)writeHandler, reserved, buffer_size);
	fflush(stderr);

	while (g_socket_read_running && g_kvm_socket_fd >= 0)
	{
		// Check if we need more buffer space
		if (buffer_size - offset < 65536)
		{
			int new_size = buffer_size * 2;
			char *new_buffer = (char*)realloc(buffer, new_size);
			if (new_buffer != NULL)
			{
				buffer = new_buffer;
				buffer_size = new_size;
				fprintf(stderr, "[KVM-SOCKET-THREAD] Expanded buffer to %d bytes\n", buffer_size);
				fflush(stderr);
			}
		}

		int bytesRead = read(g_kvm_socket_fd, buffer + offset, buffer_size - offset);
		read_count++;

		if (bytesRead <= 0)
		{
			if (bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				usleep(10000);
				continue;
			}
			fprintf(stderr, "[KVM-SOCKET-THREAD] Read error or EOF: bytesRead=%d, errno=%d, read_count=%d\n",
				bytesRead, errno, read_count);
			fflush(stderr);
			break;
		}

		if (read_count <= 5 || read_count % 100 == 0)
		{
			fprintf(stderr, "[KVM-SOCKET-THREAD] Read #%d: bytesRead=%d, offset_before=%d, total=%d\n",
				read_count, bytesRead, offset, offset + bytesRead);
			fflush(stderr);
		}

		offset += bytesRead;

		// Process complete messages (same logic as kvm_relay_StdOutHandler)
		int processed = 0;
		while (processed < offset)
		{
			int remaining = offset - processed;
			if (remaining < 4) break; // Need at least header

			unsigned short cmd = ntohs(((unsigned short*)(buffer + processed))[0]);
			int packet_size;

			if (cmd == (unsigned short)MNG_JUMBO)
			{
				// JUMBO packet: [2 bytes cmd][2 bytes=8][4 bytes data_size] + data
				// Total packet size = 8 (header) + data_size
				if (remaining < 8) break;
				int data_size = (int)ntohl(((unsigned int*)(buffer + processed))[1]);
				packet_size = 8 + data_size;

				if (message_count < 20)
				{
					fprintf(stderr, "[KVM-SOCKET-THREAD] JUMBO packet #%d: cmd=%u, data_size=%d, packet_size=%d, remaining=%d\n",
						message_count + 1, cmd, data_size, packet_size, remaining);
					fflush(stderr);
				}

				// Sanity check for JUMBO packets
				if (data_size <= 0 || data_size > 10000000) // Max 10MB for a single tile is unreasonable
				{
					fprintf(stderr, "[KVM-SOCKET-THREAD] ERROR: Invalid JUMBO data_size=%d, skipping 8 bytes\n", data_size);
					fflush(stderr);
					processed += 8;
					continue;
				}
			}
			else
			{
				// Normal packet: [2 bytes cmd][2 bytes total_size]
				packet_size = (int)ntohs(((unsigned short*)(buffer + processed))[1]);

				if (message_count < 20)
				{
					fprintf(stderr, "[KVM-SOCKET-THREAD] Normal packet #%d: cmd=%u, packet_size=%d, remaining=%d\n",
						message_count + 1, cmd, packet_size, remaining);
					fflush(stderr);
				}

				// Sanity check for normal packets
				if (packet_size <= 0 || packet_size > 65535)
				{
					fprintf(stderr, "[KVM-SOCKET-THREAD] ERROR: Invalid normal packet_size=%d, cmd=%u, skipping 4 bytes\n",
						packet_size, cmd);
					fflush(stderr);
					processed += 4;
					continue;
				}
			}

			// If packet doesn't fit in current buffer, try to expand
			if (packet_size > buffer_size)
			{
				int new_size = packet_size + 65536;
				char *new_buffer = (char*)realloc(buffer, new_size);
				if (new_buffer != NULL)
				{
					buffer = new_buffer;
					buffer_size = new_size;
					fprintf(stderr, "[KVM-SOCKET-THREAD] Expanded buffer to %d for packet_size=%d\n", buffer_size, packet_size);
					fflush(stderr);
				}
				else
				{
					fprintf(stderr, "[KVM-SOCKET-THREAD] ERROR: Cannot expand buffer for packet_size=%d\n", packet_size);
					fflush(stderr);
					break;
				}
			}

			if (packet_size > remaining) break; // Incomplete packet, wait for more data

			message_count++;
			if (message_count <= 20 || message_count % 1000 == 0)
			{
				fprintf(stderr, "[KVM-SOCKET-THREAD] Forwarding message #%d: cmd=%u, size=%d\n",
					message_count, cmd, packet_size);
				fflush(stderr);
			}

			// Forward to write handler
			writeHandler(buffer + processed, packet_size, reserved);
			processed += packet_size;
		}

		// Shift remaining data to beginning of buffer
		if (processed > 0)
		{
			if (processed < offset)
			{
				memmove(buffer, buffer + processed, offset - processed);
			}
			offset -= processed;
		}
	}

	fprintf(stderr, "[KVM-SOCKET-THREAD] Exiting, total_reads=%d, total_messages=%d, g_socket_read_running=%d\n",
		read_count, message_count, g_socket_read_running);
	fflush(stderr);
	free(buffer);
	return NULL;
}

// Accept connection from child and start read thread
static int kvm_socket_accept_and_start(void **user)
{
	fprintf(stderr, "[KVM-SOCKET-ACCEPT] Waiting for child connection on listen_fd=%d, socket_path=%s\n",
		g_kvm_listen_fd, g_kvm_socket_path);
	fflush(stderr);

	// Set a timeout for accept
	struct timeval tv;
	tv.tv_sec = 10;  // 10 second timeout
	tv.tv_usec = 0;
	int setsock_ret = setsockopt(g_kvm_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	fprintf(stderr, "[KVM-SOCKET-ACCEPT] setsockopt(SO_RCVTIMEO, 10s) returned %d\n", setsock_ret);
	fflush(stderr);

	fprintf(stderr, "[KVM-SOCKET-ACCEPT] Calling accept()...\n");
	fflush(stderr);

	g_kvm_socket_fd = accept(g_kvm_listen_fd, NULL, NULL);
	if (g_kvm_socket_fd < 0)
	{
		fprintf(stderr, "[KVM-SOCKET-ACCEPT] Accept FAILED: errno=%d (%s)\n", errno, strerror(errno));
		fflush(stderr);
		return -1;
	}

	fprintf(stderr, "[KVM-SOCKET-ACCEPT] Child connected successfully! client_fd=%d\n", g_kvm_socket_fd);
	fflush(stderr);

	// Start read thread
	fprintf(stderr, "[KVM-SOCKET-ACCEPT] Starting read thread, user=%p\n", (void*)user);
	fflush(stderr);

	g_socket_read_running = 1;
	int pthread_ret = pthread_create(&g_socket_read_thread, NULL, kvm_socket_read_thread, user);

	fprintf(stderr, "[KVM-SOCKET-ACCEPT] pthread_create returned %d, thread=%p\n",
		pthread_ret, (void*)g_socket_read_thread);
	fflush(stderr);

	return pthread_ret == 0 ? 0 : -1;
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
		else
		{
			// Standard mode: original behavior - just reset g_shutdown
			g_shutdown = 0;
		}

#if USE_LAUNCHCTL_ASUSER
		if (g_openFrameMode)
		{
			fprintf(stderr, "[KVM-LAUNCHCTL] === Starting launchctl asuser mode ===\n");
			fprintf(stderr, "[KVM-LAUNCHCTL] uid=%d, exePath=%s, binaryName=%s\n", uid, exePath, binaryName);
			fflush(stderr);

			// OpenFrame mode: Use launchctl asuser to spawn child in user's bootstrap namespace
			// This ensures TCC permissions work after daemon restart

			// Create listening socket - use fixed path
			strncpy(g_kvm_socket_path, KVM_LAUNCHCTL_SOCKET_PATH, sizeof(g_kvm_socket_path) - 1);
			fprintf(stderr, "[KVM-LAUNCHCTL] Socket path: %s\n", g_kvm_socket_path);
			fflush(stderr);

			int remove_ret = remove(g_kvm_socket_path); // Remove any stale socket
			fprintf(stderr, "[KVM-LAUNCHCTL] remove() old socket returned %d (errno=%d if failed)\n",
				remove_ret, remove_ret < 0 ? errno : 0);
			fflush(stderr);

			g_kvm_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
			if (g_kvm_listen_fd < 0)
			{
				fprintf(stderr, "[KVM-LAUNCHCTL] socket() FAILED: errno=%d (%s)\n", errno, strerror(errno));
				fflush(stderr);
				return NULL;
			}
			fprintf(stderr, "[KVM-LAUNCHCTL] socket() created fd=%d\n", g_kvm_listen_fd);
			fflush(stderr);

			struct sockaddr_un serveraddr;
			memset(&serveraddr, 0, sizeof(serveraddr));
			serveraddr.sun_family = AF_UNIX;
			strncpy(serveraddr.sun_path, g_kvm_socket_path, sizeof(serveraddr.sun_path) - 1);

			if (bind(g_kvm_listen_fd, (struct sockaddr *)&serveraddr, SUN_LEN(&serveraddr)) < 0)
			{
				fprintf(stderr, "[KVM-LAUNCHCTL] bind() FAILED: errno=%d (%s)\n", errno, strerror(errno));
				fflush(stderr);
				close(g_kvm_listen_fd);
				g_kvm_listen_fd = -1;
				return NULL;
			}
			fprintf(stderr, "[KVM-LAUNCHCTL] bind() successful\n");
			fflush(stderr);

			// Make socket accessible to user
			int chmod_ret = chmod(g_kvm_socket_path, 0777);
			fprintf(stderr, "[KVM-LAUNCHCTL] chmod(0777) returned %d\n", chmod_ret);
			fflush(stderr);

			if (listen(g_kvm_listen_fd, 1) < 0)
			{
				fprintf(stderr, "[KVM-LAUNCHCTL] listen() FAILED: errno=%d (%s)\n", errno, strerror(errno));
				fflush(stderr);
				close(g_kvm_listen_fd);
				g_kvm_listen_fd = -1;
				remove(g_kvm_socket_path);
				return NULL;
			}
			fprintf(stderr, "[KVM-LAUNCHCTL] listen() successful, socket ready at %s\n", g_kvm_socket_path);
			fflush(stderr);

			// Launch child via launchctl asuser - use -kvm0, child will detect socket and connect
			char launchCmd[1024];
			snprintf(launchCmd, sizeof(launchCmd), "launchctl asuser %d \"%s\" -kvm0 &",
				uid, exePath);

			fprintf(stderr, "[KVM-LAUNCHCTL] Executing command: %s\n", launchCmd);
			fflush(stderr);

			int ret = system(launchCmd);
			fprintf(stderr, "[KVM-LAUNCHCTL] system() returned %d\n", ret);
			fflush(stderr);

			if (ret != 0)
			{
				fprintf(stderr, "[KVM-LAUNCHCTL] system() FAILED with non-zero return!\n");
				fflush(stderr);
				close(g_kvm_listen_fd);
				g_kvm_listen_fd = -1;
				remove(g_kvm_socket_path);
				return NULL;
			}

			// Store user data for socket mode
			g_kvm_socket_user = user;
			fprintf(stderr, "[KVM-LAUNCHCTL] g_kvm_socket_user set to %p\n", g_kvm_socket_user);
			fflush(stderr);

			// Accept connection from child and start read thread
			fprintf(stderr, "[KVM-LAUNCHCTL] Calling kvm_socket_accept_and_start()...\n");
			fflush(stderr);

			int accept_ret = kvm_socket_accept_and_start(user);
			if (accept_ret < 0)
			{
				fprintf(stderr, "[KVM-LAUNCHCTL] kvm_socket_accept_and_start() FAILED: ret=%d\n", accept_ret);
				fflush(stderr);
				close(g_kvm_listen_fd);
				g_kvm_listen_fd = -1;
				remove(g_kvm_socket_path);
				return NULL;
			}
			fprintf(stderr, "[KVM-LAUNCHCTL] kvm_socket_accept_and_start() succeeded\n");
			fflush(stderr);

			// Close listening socket - we have the connection
			close(g_kvm_listen_fd);
			g_kvm_listen_fd = -1;
			fprintf(stderr, "[KVM-LAUNCHCTL] Listening socket closed\n");
			fflush(stderr);

			fprintf(stderr, "[KVM-LAUNCHCTL] === launchctl asuser mode COMPLETE! g_kvm_socket_fd=%d ===\n",
				g_kvm_socket_fd);
			fflush(stderr);

			// Return a non-NULL value to indicate success
			// In socket mode, we don't have a pipe - data comes via socket read thread
			return (void*)(intptr_t)g_kvm_socket_fd;
		}
#endif

		// Standard mode or fallback: Spawn child kvm process directly
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
		// OpenFrame mode: guard against double cleanup
		if (g_cleanup_in_progress)
		{
			fprintf(stderr, "[KVM] kvm_cleanup() already in progress, skipping\n");
			fflush(stderr);
			return;
		}
		g_cleanup_in_progress = 1;

		g_shutdown = 1;

		// Socket mode (launchctl asuser): close socket and stop read thread
		if (g_kvm_socket_fd >= 0)
		{
			fprintf(stderr, "[KVM] kvm_cleanup() closing socket %d (openframe socket mode)\n", g_kvm_socket_fd);
			fflush(stderr);

#if USE_LAUNCHCTL_ASUSER
			// Stop read thread first
			g_socket_read_running = 0;
#endif

			// Send pause command before closing
			char buffer[5];
			((unsigned short*)buffer)[0] = (unsigned short)htons((unsigned short)MNG_KVM_PAUSE);
			((unsigned short*)buffer)[1] = (unsigned short)htons((unsigned short)5);
			buffer[4] = 1;  // pause = 1
			write(g_kvm_socket_fd, buffer, 5);

			close(g_kvm_socket_fd);
			g_kvm_socket_fd = -1;

#if USE_LAUNCHCTL_ASUSER
			// Wait for read thread to exit
			if (g_socket_read_thread != (pthread_t)NULL)
			{
				pthread_join(g_socket_read_thread, NULL);
				g_socket_read_thread = (pthread_t)NULL;
			}

			// Clean up socket file
			if (g_kvm_socket_path[0] != '\0')
			{
				remove(g_kvm_socket_path);
				g_kvm_socket_path[0] = '\0';
			}
#endif

			if (g_kvm_socket_user != NULL)
			{
				ILibMemory_Free(g_kvm_socket_user);
				g_kvm_socket_user = NULL;
			}

			fprintf(stderr, "[KVM] kvm_cleanup() socket mode cleanup complete\n");
			fflush(stderr);
		}
		else if (gChildProcess != NULL && g_slavekvm > 0)
		{
			// Pipe mode: keep the child process alive for reuse
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