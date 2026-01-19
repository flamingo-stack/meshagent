#include "mac_tcc_detection.h"
#include <sqlite3.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <unistd.h>

// TCC Database Path
#define TCC_DB_PATH "/Library/Application Support/com.apple.TCC/TCC.db"

// Forward declaration
static TCC_PermissionStatus check_screen_recording_via_tcc_db(void);

/**
 * Check Full Disk Access permission
 *
 * Attempts to open TCC.db. If successful, the calling process has FDA.
 * This is the most reliable FDA check since TCC.db requires FDA to access.
 */
TCC_PermissionStatus check_fda_permission(void) {
    sqlite3 *db = NULL;

    // Try to open TCC.db read-only
    // If this succeeds, we have Full Disk Access
    int rc = sqlite3_open_v2(TCC_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL);

    if (rc == SQLITE_OK) {
        sqlite3_close(db);
        return TCC_PERMISSION_GRANTED_USER;
    }

    // Cannot open TCC.db - no FDA permission
    return TCC_PERMISSION_DENIED;
}

/**
 * Check Accessibility permission
 *
 * Uses the native Accessibility API to check if the calling process
 * has been granted Accessibility permission.
 */
TCC_PermissionStatus check_accessibility_permission(void) {
    Boolean isTrusted = AXIsProcessTrusted();
    return isTrusted ? TCC_PERMISSION_GRANTED_USER : TCC_PERMISSION_DENIED;
}

/**
 * Check Screen Recording permission via TCC database query
 *
 * Queries TCC.db directly to check if Screen Recording permission is granted.
 * This is the most reliable method - updates in real-time when user changes settings.
 * Requires FDA permission to read TCC.db.
 *
 * @return TCC_PERMISSION_GRANTED_USER if granted, TCC_PERMISSION_DENIED otherwise
 */
static TCC_PermissionStatus check_screen_recording_via_tcc_db(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    TCC_PermissionStatus result = TCC_PERMISSION_DENIED;

    // Get our bundle identifier using CoreFoundation (pure C)
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (!mainBundle) {
        return TCC_PERMISSION_DENIED;
    }

    CFStringRef bundleIdRef = CFBundleGetIdentifier(mainBundle);
    if (!bundleIdRef) {
        // No bundle ID - can't query TCC.db
        return TCC_PERMISSION_DENIED;
    }

    // Convert CFString to C string
    char bundleIdCStr[256];
    if (!CFStringGetCString(bundleIdRef, bundleIdCStr, sizeof(bundleIdCStr), kCFStringEncodingUTF8)) {
        return TCC_PERMISSION_DENIED;
    }

    // Open TCC.db
    int rc = sqlite3_open_v2(TCC_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        // Can't open TCC.db (no FDA)
        return TCC_PERMISSION_DENIED;
    }

    // Query for Screen Recording permission
    // auth_value: 0 = denied, 2 = allowed
    const char *sql = "SELECT auth_value FROM access WHERE service='kTCCServiceScreenCapture' AND client=?";

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return TCC_PERMISSION_DENIED;
    }

    // Bind bundle ID parameter
    sqlite3_bind_text(stmt, 1, bundleIdCStr, -1, SQLITE_STATIC);

    // Execute query
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int auth_value = sqlite3_column_int(stmt, 0);
        // auth_value 2 = allowed by user
        // auth_value 0 = denied
        if (auth_value == 2) {
            result = TCC_PERMISSION_GRANTED_USER;
        }
    }
    // If no row found, permission not yet requested (DENIED)

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

/**
 * Check Screen Recording permission (legacy method via window list)
 *
 * Uses CGWindowListCopyWindowInfo to check if we can see window names
 * from other processes. This method updates in REAL-TIME without requiring
 * app restart (unlike CGRequestScreenCaptureAccess which returns cached values).
 *
 * OPTIMIZED: Samples only 3 windows to minimize CPU load during polling.
 * If ANY of the 3 sampled windows has a visible name → GRANTED
 * If all 3 are NULL → DENIED
 *
 * This is the industry-standard approach used by Splashtop, TeamViewer, etc.
 * Requires macOS 10.15+
 */
static TCC_PermissionStatus check_screen_recording_via_window_list(void) {
    pid_t currentPID = getpid();

    // Get list of all on-screen windows
    CFArrayRef windowList = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) {
        return TCC_PERMISSION_DENIED;
    }

    CFIndex totalWindows = CFArrayGetCount(windowList);
    int checkedWindows = 0;
    int foundWithName = 0;
    Boolean hasPermission = false;

    // Sample up to 3 windows (skip our own PID, Dock, WindowServer)
    for (CFIndex i = 0; i < totalWindows && checkedWindows < 3; i++) {
        CFDictionaryRef window = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, i);

        // Get window owner PID
        CFNumberRef pidRef = (CFNumberRef)CFDictionaryGetValue(window, kCGWindowOwnerPID);
        if (!pidRef) continue;

        pid_t windowPID;
        CFNumberGetValue(pidRef, kCFNumberIntType, &windowPID);

        // Skip our own windows
        if (windowPID == currentPID) continue;

        // Get window owner name
        CFStringRef ownerName = (CFStringRef)CFDictionaryGetValue(window, kCGWindowOwnerName);
        if (!ownerName) continue;

        // Skip Dock and WindowServer (they always show window names)
        if (CFStringCompare(ownerName, CFSTR("Dock"), 0) == kCFCompareEqualTo ||
            CFStringCompare(ownerName, CFSTR("WindowServer"), 0) == kCFCompareEqualTo ||
            CFStringCompare(ownerName, CFSTR("Window Server"), 0) == kCFCompareEqualTo) {
            continue;
        }

        // This is a valid window to check
        checkedWindows++;

        // Get window name
        CFStringRef windowName = (CFStringRef)CFDictionaryGetValue(window, kCGWindowName);
        if (windowName && CFStringGetLength(windowName) > 0) {
            // Found a window with visible name - we have permission!
            foundWithName++;
            hasPermission = true;
            break; // Exit immediately on first success
        }
    }

    CFRelease(windowList);

    // If we checked windows but none had visible names → no permission
    if (checkedWindows > 0 && foundWithName == 0) {
        hasPermission = false;
    } else if (checkedWindows == 0) {
        // No valid windows to check (only our own, Dock, WindowServer)
        // Return DENIED but note that we couldn't verify
        hasPermission = false;
    }

    return hasPermission ? TCC_PERMISSION_GRANTED_USER : TCC_PERMISSION_DENIED;
}

/**
 * Check Screen Recording permission
 *
 * Primary method: Query TCC.db directly (requires FDA)
 * Fallback: Use window list method if TCC.db query fails
 *
 * @return TCC_PERMISSION_GRANTED_USER if granted, TCC_PERMISSION_DENIED otherwise
 */
TCC_PermissionStatus check_screen_recording_permission(void) {
    // Try TCC.db query first (most reliable, real-time updates)
    TCC_PermissionStatus result = check_screen_recording_via_tcc_db();

    // If TCC.db method returned GRANTED, we're done
    if (result == TCC_PERMISSION_GRANTED_USER) {
        return result;
    }

    // Fallback to window list method (in case TCC.db query failed or no FDA yet)
    return check_screen_recording_via_window_list();
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
