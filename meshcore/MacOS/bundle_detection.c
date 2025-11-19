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

#ifdef __APPLE__

#include "bundle_detection.h"
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <dirent.h>

int is_running_from_bundle(void)
{
    // CFBundleGetMainBundle() returns non-NULL even for standalone binaries
    // if they have embedded Info.plist, so we must check the actual path
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (!mainBundle)
    {
        return 0; // No bundle found - standalone
    }

    CFURLRef bundleURL = CFBundleCopyBundleURL(mainBundle);
    if (!bundleURL)
    {
        return 0;
    }

    CFStringRef bundlePath = CFURLCopyFileSystemPath(bundleURL, kCFURLPOSIXPathStyle);

    int is_bundle = 0;
    if (bundlePath)
    {
        char path[PATH_MAX];
        if (CFStringGetCString(bundlePath, path, PATH_MAX, kCFStringEncodingUTF8))
        {
            // Check if path ends with .app
            size_t len = strlen(path);
            is_bundle = (len > 4 && strcmp(path + len - 4, ".app") == 0);
        }
        CFRelease(bundlePath);
    }

    CFRelease(bundleURL);
    return is_bundle;
}

char* get_bundle_path(void)
{
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (!mainBundle)
    {
        return NULL;
    }

    CFURLRef bundleURL = CFBundleCopyBundleURL(mainBundle);
    if (!bundleURL)
    {
        return NULL;
    }

    CFStringRef bundlePath = CFURLCopyFileSystemPath(bundleURL, kCFURLPOSIXPathStyle);

    char* result = NULL;
    if (bundlePath)
    {
        // Get the maximum length needed for the string
        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(
            CFStringGetLength(bundlePath),
            kCFStringEncodingUTF8
        ) + 1;

        result = (char*)malloc(maxSize);
        if (result)
        {
            if (!CFStringGetCString(bundlePath, result, maxSize, kCFStringEncodingUTF8))
            {
                free(result);
                result = NULL;
            }
        }
        CFRelease(bundlePath);
    }

    CFRelease(bundleURL);
    return result;
}

void adjust_working_directory_for_bundle(void)
{
    // CRITICAL: Call this early in main() before any file operations
    if (is_running_from_bundle())
    {
        char* bundleRoot = get_bundle_path();
        if (bundleRoot)
        {
            // For bundle installations, change to the parent directory (install path)
            // This allows .msh and .db files to be found in the same location as standalone
            char* lastSlash = strrchr(bundleRoot, '/');
            if (lastSlash && lastSlash != bundleRoot)
            {
                *lastSlash = '\0';  // Truncate to get parent directory
                if (chdir(bundleRoot) == 0)
                {
                    // Restore the slash for the print statement
                    *lastSlash = '/';
                    printf("MeshAgent: Running from bundle: %s\n", bundleRoot);
                }
                else
                {
                    *lastSlash = '/';  // Restore on error too
                    fprintf(stderr, "MeshAgent: Warning: Could not change to install directory: %s\n", bundleRoot);
                }
            }
            else
            {
                fprintf(stderr, "MeshAgent: Warning: Invalid bundle path: %s\n", bundleRoot);
            }
            free(bundleRoot);
        }
        else
        {
            fprintf(stderr, "MeshAgent: Warning: Could not get bundle path\n");
        }
    }
    else
    {
        // Standalone mode - working directory already correct
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)))
        {
            printf("MeshAgent: Running as standalone binary from: %s\n", cwd);
        }
    }
}

int read_bundle_config_from_plist(const char *serviceID, char *dbPath, char *mshPath, char *logPath)
{
    if (!serviceID || !dbPath || !mshPath || !logPath)
    {
        fprintf(stderr, "MeshAgent: Invalid parameters to read_bundle_config_from_plist\n");
        return -1;
    }

    // Initialize output buffers
    dbPath[0] = '\0';
    mshPath[0] = '\0';
    logPath[0] = '\0';

    // Build plist path: /Library/Preferences/{serviceID}.plist
    char plistPath[PATH_MAX];
    snprintf(plistPath, PATH_MAX, "/Library/Preferences/%s.plist", serviceID);

    // Check if plist exists
    if (access(plistPath, F_OK) != 0)
    {
        fprintf(stderr, "MeshAgent: Configuration plist not found: %s\n", plistPath);
        return -1;
    }

    // Use CFPreferences API to read plist
    CFStringRef serviceIDRef = CFStringCreateWithCString(NULL, serviceID, kCFStringEncodingUTF8);
    if (!serviceIDRef)
    {
        fprintf(stderr, "MeshAgent: Failed to create CFString for serviceID\n");
        return -1;
    }

    CFStringRef dbKey = CFSTR("db");
    CFStringRef mshKey = CFSTR("msh");
    CFStringRef logKey = CFSTR("logPath");

    CFPropertyListRef dbValue = CFPreferencesCopyAppValue(dbKey, serviceIDRef);
    CFPropertyListRef mshValue = CFPreferencesCopyAppValue(mshKey, serviceIDRef);
    CFPropertyListRef logValue = CFPreferencesCopyAppValue(logKey, serviceIDRef);

    int result = 0;

    // Extract db path
    if (dbValue && CFGetTypeID(dbValue) == CFStringGetTypeID())
    {
        if (!CFStringGetCString((CFStringRef)dbValue, dbPath, PATH_MAX, kCFStringEncodingUTF8))
        {
            fprintf(stderr, "MeshAgent: Failed to convert 'db' value to string\n");
            result = -1;
        }
    }
    else
    {
        fprintf(stderr, "MeshAgent: Missing or invalid 'db' entry in %s\n", plistPath);
        result = -1;
    }

    // Extract msh path
    if (mshValue && CFGetTypeID(mshValue) == CFStringGetTypeID())
    {
        if (!CFStringGetCString((CFStringRef)mshValue, mshPath, PATH_MAX, kCFStringEncodingUTF8))
        {
            fprintf(stderr, "MeshAgent: Failed to convert 'msh' value to string\n");
            result = -1;
        }
    }
    else
    {
        fprintf(stderr, "MeshAgent: Missing or invalid 'msh' entry in %s\n", plistPath);
        result = -1;
    }

    // Extract log path (optional - don't fail if missing)
    if (logValue && CFGetTypeID(logValue) == CFStringGetTypeID())
    {
        if (!CFStringGetCString((CFStringRef)logValue, logPath, PATH_MAX, kCFStringEncodingUTF8))
        {
            fprintf(stderr, "MeshAgent: Failed to convert 'logPath' value to string\n");
            // Don't set result = -1 since logPath is optional
        }
    }
    // Note: logPath is optional, so no error if missing

    // Cleanup
    if (dbValue) CFRelease(dbValue);
    if (mshValue) CFRelease(mshValue);
    if (logValue) CFRelease(logValue);
    CFRelease(serviceIDRef);

    return result;
}

int get_service_id_from_launchdaemon(const char *exePath, char *serviceID)
{
    if (!exePath || !serviceID)
    {
        fprintf(stderr, "MeshAgent: Invalid parameters to get_service_id_from_launchdaemon\n");
        return -1;
    }

    // Initialize output
    strcpy(serviceID, "meshagent"); // Default fallback

    // Search /Library/LaunchDaemons for plist with matching ProgramArguments[0]
    // This implementation uses PlistBuddy via popen, similar to mac_kvm.c:kvm_read_serviceid_from_plist()

    DIR *dir = opendir("/Library/LaunchDaemons");
    if (!dir)
    {
        fprintf(stderr, "MeshAgent: Cannot open /Library/LaunchDaemons\n");
        return 0; // Use default, not fatal
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        // Skip non-plist files
        if (strstr(entry->d_name, ".plist") == NULL)
        {
            continue;
        }

        char plistPath[PATH_MAX];
        snprintf(plistPath, sizeof(plistPath), "/Library/LaunchDaemons/%s", entry->d_name);

        // Extract ProgramArguments[0] using PlistBuddy
        char command[PATH_MAX + 100];
        snprintf(command, sizeof(command),
                 "/usr/libexec/PlistBuddy -c 'Print :ProgramArguments:0' '%s' 2>/dev/null",
                 plistPath);

        FILE *pipe = popen(command, "r");
        if (!pipe)
        {
            continue;
        }

        char binPath[PATH_MAX];
        if (fgets(binPath, sizeof(binPath), pipe) != NULL)
        {
            // Remove trailing newline
            binPath[strcspn(binPath, "\n")] = 0;

            // Check if this matches our binary path
            if (strcmp(binPath, exePath) == 0)
            {
                pclose(pipe);

                // Found matching plist! Now extract Label
                snprintf(command, sizeof(command),
                         "/usr/libexec/PlistBuddy -c 'Print :Label' '%s' 2>/dev/null",
                         plistPath);

                pipe = popen(command, "r");
                if (pipe != NULL)
                {
                    char label[512];
                    if (fgets(label, sizeof(label), pipe) != NULL)
                    {
                        label[strcspn(label, "\n")] = 0;
                        strncpy(serviceID, label, 511);
                        serviceID[511] = '\0';
                        pclose(pipe);
                        closedir(dir);
                        printf("MeshAgent: Found serviceID from LaunchDaemon: %s\n", serviceID);
                        return 0;
                    }
                    pclose(pipe);
                }
            }
        }
        else
        {
            pclose(pipe);
        }
    }

    closedir(dir);
    printf("MeshAgent: Using default serviceID: %s\n", serviceID);
    return 0;
}

#endif /* __APPLE__ */
