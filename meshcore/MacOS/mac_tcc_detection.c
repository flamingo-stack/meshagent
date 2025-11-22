#include "mac_tcc_detection.h"
#include <sqlite3.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <unistd.h>

// TCC Database Path
#define TCC_DB_PATH "/Library/Application Support/com.apple.TCC/TCC.db"

/**
 * Check Full Disk Access permission
 *
 * Attempts to open TCC.db. If successful, the calling process has FDA.
 * This is the most reliable FDA check since TCC.db requires FDA to access.
 */
TCC_PermissionStatus check_fda_permission(void) {
    sqlite3 *db = NULL;

    printf("[TCC-API] check_fda_permission: Attempting to open TCC.db at: %s\n", TCC_DB_PATH);
    // Try to open TCC.db read-only
    // If this succeeds, we have Full Disk Access
    int rc = sqlite3_open_v2(TCC_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL);

    printf("[TCC-API] check_fda_permission: sqlite3_open_v2 returned: %d (SQLITE_OK=%d)\n", rc, SQLITE_OK);

    if (rc == SQLITE_OK) {
        sqlite3_close(db);
        printf("[TCC-API] check_fda_permission: GRANTED (TCC_PERMISSION_GRANTED_USER=%d)\n", TCC_PERMISSION_GRANTED_USER);
        return TCC_PERMISSION_GRANTED_USER;
    }

    // Cannot open TCC.db - no FDA permission
    printf("[TCC-API] check_fda_permission: DENIED (TCC_PERMISSION_DENIED=%d)\n", TCC_PERMISSION_DENIED);
    return TCC_PERMISSION_DENIED;
}

/**
 * Check Accessibility permission
 *
 * Uses the native Accessibility API to check if the calling process
 * has been granted Accessibility permission.
 */
TCC_PermissionStatus check_accessibility_permission(void) {
    printf("[TCC-API] check_accessibility_permission: Calling AXIsProcessTrusted()\n");
    Boolean isTrusted = AXIsProcessTrusted();
    printf("[TCC-API] check_accessibility_permission: AXIsProcessTrusted returned: %d\n", isTrusted);

    TCC_PermissionStatus result = isTrusted ? TCC_PERMISSION_GRANTED_USER : TCC_PERMISSION_DENIED;
    printf("[TCC-API] check_accessibility_permission: Returning %d (%s)\n",
           result, isTrusted ? "GRANTED" : "DENIED");
    return result;
}

/**
 * Check Screen Recording permission
 *
 * Uses CGWindowListCopyWindowInfo to check if we can see window names
 * from other processes. This method updates in REAL-TIME without requiring
 * app restart (unlike CGRequestScreenCaptureAccess which returns cached values).
 *
 * This is the industry-standard approach used by Splashtop, TeamViewer, etc.
 * Requires macOS 10.15+
 */
TCC_PermissionStatus check_screen_recording_permission(void) {
    printf("[TCC-API] ========================================\n");
    printf("[TCC-API] check_screen_recording_permission: START\n");
    printf("[TCC-API] ========================================\n");

    pid_t currentPID = getpid();
    printf("[TCC-API] Current process PID: %d\n", currentPID);

    // Get list of all on-screen windows
    CFArrayRef windowList = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) {
        printf("[TCC-API] ERROR: CGWindowListCopyWindowInfo failed - returning DENIED\n");
        printf("[TCC-API] ========================================\n");
        return TCC_PERMISSION_DENIED;
    }

    CFIndex totalWindows = CFArrayGetCount(windowList);
    printf("[TCC-API] Total windows returned by CGWindowListCopyWindowInfo: %ld\n", (long)totalWindows);

    Boolean hasPermission = false;
    int totalChecked = 0;
    int skippedOwnWindows = 0;
    int skippedDock = 0;
    int skippedWindowServer = 0;
    int skippedNoOwnerName = 0;
    int skippedNoWindowName = 0;
    int validNamedWindows = 0;

    printf("[TCC-API] ----------------------------------------\n");
    printf("[TCC-API] Analyzing windows...\n");
    printf("[TCC-API] ----------------------------------------\n");

    // Check if we can see window names from other processes
    for (CFIndex i = 0; i < totalWindows; i++) {
        CFDictionaryRef window = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, i);
        totalChecked++;

        // Get window owner PID
        CFNumberRef pidRef = (CFNumberRef)CFDictionaryGetValue(window, kCGWindowOwnerPID);
        if (!pidRef) {
            printf("[TCC-API] Window %ld: Skipped (no PID)\n", (long)i);
            continue;
        }

        pid_t windowPID;
        CFNumberGetValue(pidRef, kCFNumberIntType, &windowPID);

        // Skip our own windows
        if (windowPID == currentPID) {
            skippedOwnWindows++;
            continue;
        }

        // Get window owner name
        CFStringRef ownerName = (CFStringRef)CFDictionaryGetValue(window, kCGWindowOwnerName);
        char ownerNameBuf[256] = "Unknown";
        if (ownerName) {
            CFStringGetCString(ownerName, ownerNameBuf, sizeof(ownerNameBuf), kCFStringEncodingUTF8);
        } else {
            skippedNoOwnerName++;
            continue;
        }

        // Skip Dock (it always shows window names for desktop icons)
        if (CFStringCompare(ownerName, CFSTR("Dock"), 0) == kCFCompareEqualTo) {
            skippedDock++;
            printf("[TCC-API] Window %ld: PID %d (%s) - SKIPPED (Dock)\n", (long)i, windowPID, ownerNameBuf);
            continue;
        }

        // Skip WindowServer (with or without space)
        if (CFStringCompare(ownerName, CFSTR("WindowServer"), 0) == kCFCompareEqualTo ||
            CFStringCompare(ownerName, CFSTR("Window Server"), 0) == kCFCompareEqualTo) {
            skippedWindowServer++;
            printf("[TCC-API] Window %ld: PID %d (%s) - SKIPPED (WindowServer)\n", (long)i, windowPID, ownerNameBuf);
            continue;
        }

        // Get window name
        CFStringRef windowName = (CFStringRef)CFDictionaryGetValue(window, kCGWindowName);
        if (!windowName) {
            skippedNoWindowName++;
            printf("[TCC-API] Window %ld: PID %d (%s) - No window name\n", (long)i, windowPID, ownerNameBuf);
            continue;
        }

        CFIndex nameLength = CFStringGetLength(windowName);
        if (nameLength == 0) {
            skippedNoWindowName++;
            printf("[TCC-API] Window %ld: PID %d (%s) - Empty window name\n", (long)i, windowPID, ownerNameBuf);
            continue;
        }

        // We found a named window from another process!
        char windowNameBuf[256];
        CFStringGetCString(windowName, windowNameBuf, sizeof(windowNameBuf), kCFStringEncodingUTF8);

        validNamedWindows++;
        hasPermission = true;

        printf("[TCC-API] Window %ld: PID %d (%s) - FOUND NAMED WINDOW: \"%s\" ✓\n",
               (long)i, windowPID, ownerNameBuf, windowNameBuf);

        // Only log first 5 valid windows to avoid spam
        if (validNamedWindows >= 5) {
            printf("[TCC-API] ... (stopping detailed logging after 5 valid windows)\n");
            break;
        }
    }

    CFRelease(windowList);

    printf("[TCC-API] ----------------------------------------\n");
    printf("[TCC-API] SUMMARY:\n");
    printf("[TCC-API] ----------------------------------------\n");
    printf("[TCC-API] Total windows checked:        %d\n", totalChecked);
    printf("[TCC-API] Skipped (own process):        %d\n", skippedOwnWindows);
    printf("[TCC-API] Skipped (Dock):               %d\n", skippedDock);
    printf("[TCC-API] Skipped (WindowServer):       %d\n", skippedWindowServer);
    printf("[TCC-API] Skipped (no owner name):      %d\n", skippedNoOwnerName);
    printf("[TCC-API] Skipped (no/empty window name): %d\n", skippedNoWindowName);
    printf("[TCC-API] VALID named windows found:    %d\n", validNamedWindows);
    printf("[TCC-API] ----------------------------------------\n");

    TCC_PermissionStatus result = hasPermission ? TCC_PERMISSION_GRANTED_USER : TCC_PERMISSION_DENIED;
    printf("[TCC-API] RESULT: %s\n", hasPermission ? "GRANTED (found named windows)" : "DENIED (no named windows)");
    printf("[TCC-API] ========================================\n\n");

    return result;
}

/**
 * Check all TCC permissions
 *
 * Convenience function to check all three permissions at once.
 */
TCC_AllPermissions check_all_permissions(void) {
    TCC_AllPermissions result;
    result.fda = check_fda_permission();
    result.accessibility = check_accessibility_permission();
    result.screen_recording = check_screen_recording_permission();
    return result;
}
