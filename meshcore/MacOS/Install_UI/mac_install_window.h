#ifndef MAC_INSTALL_WINDOW_H
#define MAC_INSTALL_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Installation mode selection
 */
typedef enum {
    MeshAgent_INSTALL_MODE_UPGRADE = 0,
    MeshAgent_INSTALL_MODE_NEW = 1
} MeshAgent_InstallMode;

/**
 * Installation result structure
 */
typedef struct {
    MeshAgent_InstallMode mode;
    char installPath[1024];
    char mshFilePath[1024];
    int enableDisableUpdate;  // 1 to enable, 0 to disable
    int cancelled;  // 1 if user cancelled, 0 if user clicked Install
} MeshAgent_InstallResult;

/**
 * Display the MeshAgent Installation Assistant
 *
 * Shows a modal window allowing the user to choose between:
 * - Upgrade existing installation (browse for existing meshagent location)
 * - New installation (browse for install folder + .msh file)
 *
 * Returns:
 *   MeshAgent_InstallResult structure with user's selections
 *   cancelled=1 if user clicked Cancel
 *   cancelled=0 if user clicked Install/Upgrade
 */
MeshAgent_InstallResult MeshAgent_show_install_assistant_window(void);

#ifdef __cplusplus
}
#endif

#endif // MAC_INSTALL_WINDOW_H

