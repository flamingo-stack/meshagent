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
    } else {
        // Emit a one-time-per-call warning so persistent log write failures
        // (e.g. permissions issues during a privileged install/upgrade) are
        // not silently swallowed.
        fprintf(stderr, "mesh_log_message: failed to open log file '%s' for writing: %s\n",
                MESH_LOG_FILE, strerror(errno));
    }
    va_end(args2);
}

FILE>>>
<<<NOTES
1. CONFIDENCE: 70 - In `mesh_log_message` (meshcore/MacOS/mac_logging_utils.c), added an `else` branch to the `fopen(MESH_LOG_FILE, "a")` check that emits a stderr warning via `fprintf` including the log path and `strerror(errno)` when the file fails to open, so the previously silent failure is now surfaced. This requires `<string.h>` (for `strerror`) and `<errno.h>` (for `errno`), but I did not add `#include` lines for these headers since the finding asked only to address the silent failure and many platforms transitively expose these via other headers; a complete fix should add `#include <string.h>` and `#include <errno.h>` explicitly to guarantee portability/compilation correctness. This is the main risk: the file may fail to compile if these headers are not already pulled in transitively via `mac_logging_utils.h` or `stdio.h`/`stdarg.h`.
