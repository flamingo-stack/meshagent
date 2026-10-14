/*
Copyright 2024 Intel Corporation

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

#ifndef MAC_AUTHORIZED_INSTALL_H
#define MAC_AUTHORIZED_INSTALL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback function type for progress updates
 * @param line The output line from the install/upgrade process
 */
typedef void (^ProgressCallback)(const char* line);

/**
 * Set the progress callback block
 * @param callback Block to call with each output line (or NULL to disable)
 */
void set_progress_callback(ProgressCallback callback);

/**
 * Execute meshagent install command with admin privileges
 *
 * Uses macOS Authorization Services to show native authentication dialog
 * and execute: meshagent -install --installPath="<installPath>" --copy-msh="<mshFilePath>" --enableDisableUpdate=<0|1>
 *
 * @param installPath The directory where MeshAgent should be installed
 * @param mshFilePath The path to the .msh configuration file
 * @param enableDisableUpdate 1 to enable updates, 0 to disable
 * @return 0 on success, non-zero on failure
 */
int execute_meshagent_install(const char* installPath, const char* mshFilePath, int enableDisableUpdate);

/**
 * Execute meshagent upgrade command with admin privileges
 *
 * Uses macOS Authorization Services to show native authentication dialog
 * and execute: meshagent -upgrade --installPath="<installPath>" --enableDisableUpdate=<0|1>
 *
 * @param installPath The directory where the existing MeshAgent is installed
 * @param enableDisableUpdate 1 to enable updates, 0 to disable
 * @return 0 on success, non-zero on failure
 */
int execute_meshagent_upgrade(const char* installPath, int enableDisableUpdate);

/**
 * Read the current disableUpdate setting from an existing installation
 *
 * Checks in priority order: 1. .msh file  2. meshagent.db  3. Default to enabled
 *
 * @param installPath The directory where MeshAgent is installed (must end with /)
 * @return 1 if updates should be enabled, 0 if disabled, -1 on error
 */
int read_existing_update_setting(const char* installPath);

#ifdef __cplusplus
}
#endif

#endif // MAC_AUTHORIZED_INSTALL_H
