/*
Copyright 2006 - 2025 Intel Corporation

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

#ifndef MAC_INSTALL_WINDOW_H
#define MAC_INSTALL_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Installation mode selection
 */
typedef enum {
    INSTALL_MODE_UPGRADE = 0,
    INSTALL_MODE_NEW = 1
} InstallMode;

/**
 * Installation result structure
 */
typedef struct {
    InstallMode mode;
    char installPath[1024];
    char mshFilePath[1024];
    int enableDisableUpdate;  // 1 to enable, 0 to disable
    int cancelled;  // 1 if user cancelled, 0 if user clicked Install
} InstallResult;

/**
 * Display the MeshAgent Installation Assistant
 *
 * Shows a modal window allowing the user to choose between:
 * - Upgrade existing installation (browse for existing meshagent location)
 * - New installation (browse for install folder + .msh file)
 *
 * Returns:
 *   InstallResult structure with user's selections
 *   cancelled=1 if user clicked Cancel
 *   cancelled=0 if user clicked Install/Upgrade
 */
InstallResult show_install_assistant_window(void);

#ifdef __cplusplus
}
#endif

#endif // MAC_INSTALL_WINDOW_H

