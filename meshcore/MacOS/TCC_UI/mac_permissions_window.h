#ifndef MAC_PERMISSIONS_WINDOW_H
#define MAC_PERMISSIONS_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Display the TCC permissions window
 *
 * Shows a modal window with the required TCC permissions:
 * - Accessibility
 * - Full Disk Access
 * - Screen & System Audio Recording
 *
 * Returns:
 *   0 if user clicked "Finish"
 *   1 if user wants "Do not remind me again"
 */
int show_tcc_permissions_window(void);

/**
 * Display the TCC permissions window asynchronously (non-blocking)
 *
 * Spawns a child process with "-tccCheck" flag to show the permissions UI.
 * Returns immediately (non-blocking). The child process will write the
 * "Do not remind me again" preference (0 or 1) to a pipe when the window is closed.
 *
 * Parameters:
 *   exe_path - Path to the meshagent executable (for re-execing self)
 *
 * Returns:
 *   File descriptor for reading result from child (read end of pipe)
 *   -1 on error (e.g., pipe creation failed, TCC UI already running)
 */
int show_tcc_permissions_window_async(const char* exe_path);

#ifdef __cplusplus
}
#endif

#endif // MAC_PERMISSIONS_WINDOW_H
