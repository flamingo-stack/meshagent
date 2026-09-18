/*
Copyright 2025 Intel Corporation

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

/*
Shared logging utility for macOS components

Provides centralized logging to both stderr and a file for troubleshooting
installation, upgrade, and TCC permission issues.
*/

#include "mac_logging_utils.h"
#include <stdio.h>
#include <stdarg.h>

/**
 * Log a message to both stderr and the log file
 *
 * This function duplicates the output to both destinations to ensure:
 * 1. Real-time visibility in console/terminal (stderr)
 * 2. Persistent record for post-mortem debugging (log file)
 */
void mesh_log_message(const char* format, ...) {
    va_list args1, args2;
    va_start(args1, format);
    va_copy(args2, args1);

    // Log to stderr for real-time monitoring
    vfprintf(stderr, format, args1);
    va_end(args1);

    // Log to file for persistent troubleshooting
    FILE* logFile = fopen(MESH_LOG_FILE, "a");
    if (logFile) {
        vfprintf(logFile, format, args2);
        fflush(logFile);  // Ensure immediate write (important for crash debugging)
        fclose(logFile);
    }
    va_end(args2);
}

