/*
Copyright 2025

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

#ifndef MACOS_BUNDLE_DETECTION_H
#define MACOS_BUNDLE_DETECTION_H

#ifdef __APPLE__

/**
 * Detect if the binary is running from a macOS application bundle (.app)
 *
 * Uses CoreFoundation to reliably detect bundle execution by checking if the
 * bundle path ends with ".app". This distinguishes between:
 * - Standalone binaries with embedded Info.plist (returns 0)
 * - Actual application bundles (returns 1)
 *
 * @return 1 if running from a .app bundle, 0 if standalone binary
 */
int is_running_from_bundle(void);

/**
 * Get the application bundle root directory path
 *
 * Returns the full path to the .app directory (e.g., "/Applications/MeshAgent.app")
 *
 * @return Dynamically allocated string containing bundle path, or NULL on failure
 *         Caller must free() the returned string when done
 */
char* get_bundle_path(void);

/**
 * Adjust working directory based on bundle status
 *
 * If running from a bundle:
 *   - Changes working directory to the bundle root (.app directory)
 *   - Prints "MeshAgent: Running from bundle: <path>"
 *
 * If running as standalone binary:
 *   - Leaves working directory unchanged
 *   - Prints "MeshAgent: Running as standalone binary from: <cwd>"
 *
 * This should be called early in main() before any file I/O operations that
 * depend on relative paths.
 */
void adjust_working_directory_for_bundle(void);

/**
 * Read configuration paths from macOS preferences plist
 *
 * Reads /Library/Preferences/{serviceID}.plist and extracts:
 * - db: full path to meshagent.db
 * - msh: full path to meshagent.msh
 *
 * @param serviceID The service identifier (e.g., "meshagent" or "meshagent.companyname")
 * @param dbPath Output buffer for database path (PATH_MAX size)
 * @param mshPath Output buffer for msh path (PATH_MAX size)
 * @return 0 on success, -1 on error (plist not found or keys missing)
 */
int read_bundle_config_from_plist(const char *serviceID, char *dbPath, char *mshPath, char *logPath);

/**
 * Determine serviceID for current installation
 *
 * Searches /Library/LaunchDaemons for plist files with ProgramArguments[0]
 * matching the current executable path, then extracts the Label field.
 *
 * Priority:
 * 1. Read from LaunchDaemon plist Label (searches for binary path match)
 * 2. Default to "meshagent"
 *
 * @param exePath Path to current binary
 * @param serviceID Output buffer (512 bytes minimum)
 * @return 0 on success, -1 on error
 */
int get_service_id_from_launchdaemon(const char *exePath, char *serviceID);

#endif /* __APPLE__ */

#endif /* MACOS_BUNDLE_DETECTION_H */
