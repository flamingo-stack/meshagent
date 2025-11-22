#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import "mac_permissions_window.h"
#import "../mac_tcc_detection.h"
#include "../../../microstack/ILibSimpleDataStore.h"
#include "../../../microstack/ILibProcessPipe.h"
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

// Lock file to prevent multiple TCC UI processes
#define TCC_LOCK_FILE "/tmp/meshagent_tcccheck.lock"

// Button tags for identification
#define BUTTON_TAG_ACCESSIBILITY 1
#define BUTTON_TAG_FDA 2
#define BUTTON_TAG_SCREEN_RECORDING 3

// Helper functions for lock file management
static int is_process_running(pid_t pid) {
    return kill(pid, 0) == 0;
}

static int is_tcc_ui_running(void) {
    FILE* f = fopen(TCC_LOCK_FILE, "r");
    if (f == NULL) {
        return 0; // No lock file
    }

    pid_t pid;
    if (fscanf(f, "%d", &pid) == 1) {
        fclose(f);
        if (is_process_running(pid)) {
            return 1; // Process is running
        } else {
            // Stale lock, remove it
            unlink(TCC_LOCK_FILE);
            return 0;
        }
    }

    fclose(f);
    return 0;
}

static void create_lock_file(void) {
    FILE* f = fopen(TCC_LOCK_FILE, "w");
    if (f != NULL) {
        fprintf(f, "%d", getpid());
        fclose(f);
    }
}

static void remove_lock_file(void) {
    unlink(TCC_LOCK_FILE);
}

// Button action handler class
@interface TCCButtonHandler : NSObject
@property (nonatomic, assign) NSView *contentView;
@property (nonatomic, strong) NSTimer *updateTimer;

- (instancetype)initWithContentView:(NSView*)view;
- (void)openAccessibilitySettings:(id)sender;
- (void)openFullDiskAccessSettings:(id)sender;
- (void)openScreenRecordingSettings:(id)sender;
- (void)updatePermissionStatus;
- (void)startPeriodicUpdates;
- (void)stopPeriodicUpdates;
@end

@implementation TCCButtonHandler

- (instancetype)initWithContentView:(NSView*)view {
    self = [super init];
    if (self) {
        _contentView = view;
        _updateTimer = nil;
    }
    return self;
}

- (void)dealloc {
    [self stopPeriodicUpdates];
    [super dealloc];
}

- (void)replaceButtonWithSuccessIcon:(NSButton*)button {
    // Check if button is already showing checkmark (avoid re-applying transformation)
    if ([button image] != nil) {
        return; // Already converted to checkmark
    }

    if (@available(macOS 11.0, *)) {
        // Get button's tag to determine original position
        NSInteger tag = [button tag];
        CGFloat yPos;
        if (tag == BUTTON_TAG_ACCESSIBILITY) {
            yPos = 235;
        } else if (tag == BUTTON_TAG_FDA) {
            yPos = 165;
        } else if (tag == BUTTON_TAG_SCREEN_RECORDING) {
            yPos = 100;
        } else {
            return;
        }

        // Create checkmark icon (32×32 with 28pt icon)
        NSImageSymbolConfiguration* config = [NSImageSymbolConfiguration configurationWithPointSize:28 weight:NSFontWeightRegular];
        NSImage* icon = [NSImage imageWithSystemSymbolName:@"checkmark.circle.fill" accessibilityDescription:@"Permission Granted"];
        NSImage* configuredIcon = [icon imageWithSymbolConfiguration:config];

        // Change button to show checkmark icon instead of text
        [button setImage:configuredIcon];
        [button setImagePosition:NSImageOnly];
        [button setContentTintColor:[NSColor systemGreenColor]];
        [button setTitle:@""];
        [button setBordered:NO];
        [button setBezelStyle:NSBezelStyleRegularSquare];

        // Center checkmark where the button used to be (shifted down 12px)
        // Button was at x=440 with width=140, so center is at 440 + 70 = 510
        // Checkmark is 32 wide, so center it: 510 - 16 = 494
        [button setFrame:NSMakeRect(494, yPos - 20 + (28 - 32) / 2, 32, 32)];
    } else {
        // Fallback for older macOS
        [button setTitle:@"✓ Granted"];
    }
}

- (void)showButton:(NSButton*)button {
    // Check if button is already showing "Open Settings" (avoid re-applying transformation)
    if ([button image] == nil && [[button title] isEqualToString:@"Open Settings"]) {
        return; // Already showing button
    }

    // Restore button to original "Open Settings" appearance
    [button setTitle:@"Open Settings"];
    [button setImage:nil];
    [button setImagePosition:NSImageLeft];
    [button setBordered:YES];
    [button setBezelStyle:NSBezelStyleRounded];

    // Restore original size and position
    NSInteger tag = [button tag];
    CGFloat yPos;
    if (tag == BUTTON_TAG_ACCESSIBILITY) {
        yPos = 235;
    } else if (tag == BUTTON_TAG_FDA) {
        yPos = 165;
    } else if (tag == BUTTON_TAG_SCREEN_RECORDING) {
        yPos = 100;
    } else {
        return;
    }

    [button setFrame:NSMakeRect(440, yPos - 20, 140, 28)];
}

- (void)updatePermissionStatus {
    // Use unsafe_unretained instead of weak for MRC compatibility
    __unsafe_unretained TCCButtonHandler *unsafeSelf = self;

    // Check permissions on background thread to avoid blocking UI
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        @autoreleasepool {
            // Check if window is still valid (unsafe but necessary without ARC __weak)
            if (!unsafeSelf || !unsafeSelf.contentView) {
                NSLog(@"[TCC-UI] Window deallocated during permission check, aborting");
                return;
            }

            // Check all permissions for the calling process
            TCC_PermissionStatus accessibility = check_accessibility_permission();
            TCC_PermissionStatus fda = check_fda_permission();
            TCC_PermissionStatus screen_recording = check_screen_recording_permission();

            // Update UI on main thread
            dispatch_async(dispatch_get_main_queue(), ^{
                // Check again if window is still valid
                if (!unsafeSelf || !unsafeSelf.contentView) {
                    NSLog(@"[TCC-UI] Window deallocated before UI update, aborting");
                    return;
                }

                // Snapshot subviews to avoid issues if view hierarchy changes during iteration
                NSArray *subviews = [unsafeSelf.contentView subviews];
                for (NSView *subview in subviews) {
                if ([subview isKindOfClass:[NSButton class]]) {
                    NSButton *button = (NSButton*)subview;
                    NSInteger tag = [button tag];

                    TCC_PermissionStatus status = TCC_PERMISSION_NOT_DETERMINED;
                    if (tag == BUTTON_TAG_ACCESSIBILITY) {
                        status = accessibility;
                    } else if (tag == BUTTON_TAG_FDA) {
                        status = fda;
                    } else if (tag == BUTTON_TAG_SCREEN_RECORDING) {
                        status = screen_recording;
                    } else {
                        continue; // Not a permission button
                    }

                    // Update button display based on status
                    if (status == TCC_PERMISSION_GRANTED_USER || status == TCC_PERMISSION_GRANTED_MDM) {
                        [unsafeSelf replaceButtonWithSuccessIcon:button];
                    } else {
                        [unsafeSelf showButton:button];
                    }
                }
            }
            });
        }
    });
}

- (void)startPeriodicUpdates {
    // Check immediately
    [self updatePermissionStatus];

    // Use NSDistributedNotificationCenter to get real-time updates for Accessibility
    // This is how Splashtop and other apps do it - no polling needed!
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:self
        selector:@selector(accessibilityPermissionChanged:)
        name:@"com.apple.accessibility.api"
        object:nil
        suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];

    NSLog(@"[TCC-UI] Registered for accessibility permission change notifications");

    // For Screen Recording and FDA, we need light polling since there's no notification
    // Use 3-second interval since we have real-time updates for Accessibility via notification
    __unsafe_unretained TCCButtonHandler *unsafeSelf = self;
    self.updateTimer = [NSTimer timerWithTimeInterval:3.0
                                               repeats:YES
                                                 block:^(NSTimer *timer) {
        if (unsafeSelf) {
            // Only check Screen Recording and FDA (Accessibility updated via notification)
            [unsafeSelf checkScreenRecordingAndFDA];
        } else {
            NSLog(@"[TCC-UI] Window deallocated, invalidating timer");
            [timer invalidate];
        }
    }];

    [[NSRunLoop currentRunLoop] addTimer:self.updateTimer forMode:NSRunLoopCommonModes];
}

// Called when Accessibility permission changes (real-time notification)
- (void)accessibilityPermissionChanged:(NSNotification *)notification {
    NSLog(@"[TCC-UI] Accessibility permission changed notification received");

    // Use unsafe_unretained instead of weak for MRC compatibility
    __unsafe_unretained TCCButtonHandler *unsafeSelf = self;

    // Small delay to let the change settle, then check on background thread
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.2 * NSEC_PER_SEC)), dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        @autoreleasepool {
            // Check if window is still valid
            if (!unsafeSelf || !unsafeSelf.contentView) {
                NSLog(@"[TCC-UI] Window deallocated during accessibility check, aborting");
                return;
            }

            TCC_PermissionStatus accessibility = check_accessibility_permission();

            // Update UI on main thread
            dispatch_async(dispatch_get_main_queue(), ^{
                // Check again if window is still valid before UI update
                if (!unsafeSelf || !unsafeSelf.contentView) {
                    NSLog(@"[TCC-UI] Window deallocated before accessibility UI update, aborting");
                    return;
                }

                // Snapshot subviews to avoid issues if view hierarchy changes during iteration
                NSArray *subviews = [unsafeSelf.contentView subviews];
                for (NSView *subview in subviews) {
                    if ([subview isKindOfClass:[NSButton class]]) {
                        NSButton *button = (NSButton*)subview;
                        if ([button tag] == BUTTON_TAG_ACCESSIBILITY) {
                            if (accessibility == TCC_PERMISSION_GRANTED_USER || accessibility == TCC_PERMISSION_GRANTED_MDM) {
                                [unsafeSelf replaceButtonWithSuccessIcon:button];
                            } else {
                                [unsafeSelf showButton:button];
                            }
                            break;
                        }
                    }
                }
            });
        }
    });
}

// Check only Screen Recording and FDA (called by timer)
- (void)checkScreenRecordingAndFDA {
    // Use unsafe_unretained instead of weak for MRC compatibility
    __unsafe_unretained TCCButtonHandler *unsafeSelf = self;

    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        @autoreleasepool {
            // Check if window is still valid before doing expensive work
            if (!unsafeSelf || !unsafeSelf.contentView) {
                NSLog(@"[TCC-UI] Window deallocated during permission check, aborting");
                return;
            }

            TCC_PermissionStatus fda = check_fda_permission();
            TCC_PermissionStatus screen_recording = check_screen_recording_permission();

            dispatch_async(dispatch_get_main_queue(), ^{
                // Check again if window is still valid before UI update
                if (!unsafeSelf || !unsafeSelf.contentView) {
                    NSLog(@"[TCC-UI] Window deallocated before UI update, aborting");
                    return;
                }

                // Snapshot subviews to avoid issues if view hierarchy changes during iteration
                NSArray *subviews = [unsafeSelf.contentView subviews];
                for (NSView *subview in subviews) {
                if ([subview isKindOfClass:[NSButton class]]) {
                    NSButton *button = (NSButton*)subview;
                    NSInteger tag = [button tag];

                    TCC_PermissionStatus status = TCC_PERMISSION_NOT_DETERMINED;
                    if (tag == BUTTON_TAG_FDA) {
                        status = fda;
                    } else if (tag == BUTTON_TAG_SCREEN_RECORDING) {
                        status = screen_recording;
                    } else {
                        continue;
                    }

                    if (status == TCC_PERMISSION_GRANTED_USER || status == TCC_PERMISSION_GRANTED_MDM) {
                        [unsafeSelf replaceButtonWithSuccessIcon:button];
                    } else {
                        [unsafeSelf showButton:button];
                    }
                }
            }
            });
        }
    });
}

- (void)stopPeriodicUpdates {
    NSLog(@"[TCC-UI] stopPeriodicUpdates called");

    // Remove notification observer
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
    NSLog(@"[TCC-UI] Removed notification observers");

    if (self.updateTimer) {
        [self.updateTimer invalidate];
        self.updateTimer = nil;
        NSLog(@"[TCC-UI] Timer invalidated and cleared");
    }
}

- (void)openAccessibilitySettings:(id)sender {
    NSLog(@"[TCC-UI] openAccessibilitySettings clicked");

    // Trigger the system permission prompt to add MeshAgent to the Accessibility list
    if (__builtin_available(macOS 10.9, *)) {
        NSLog(@"[TCC-UI] Calling AXIsProcessTrustedWithOptions on background thread");
        // Call on background thread to avoid blocking UI (API blocks until user responds)
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            @autoreleasepool {
                const void *keys[] = { kAXTrustedCheckOptionPrompt };
                const void *values[] = { kCFBooleanTrue };
                CFDictionaryRef options = CFDictionaryCreate(
                    kCFAllocatorDefault,
                    keys,
                    values,
                    1,
                    &kCFCopyStringDictionaryKeyCallBacks,
                    &kCFTypeDictionaryValueCallBacks);

                // This will show the system dialog: "MeshAgent.app would like to control this computer using accessibility features"
                Boolean result = AXIsProcessTrustedWithOptions(options);
                NSLog(@"[TCC-UI] AXIsProcessTrustedWithOptions returned: %d", result);
                CFRelease(options);
            }
        });
    }

    // Also open System Settings so user can see MeshAgent was added to the list
    NSLog(@"[TCC-UI] Opening System Settings for Accessibility");
    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

- (void)openFullDiskAccessSettings:(id)sender {
    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

- (void)openScreenRecordingSettings:(id)sender {
    NSLog(@"[TCC-UI] openScreenRecordingSettings clicked");

    // Trigger the system permission prompt to add MeshAgent to the Screen Recording list
    if (__builtin_available(macOS 10.15, *)) {
        // Check current permission status
        Boolean hasPermission = CGPreflightScreenCaptureAccess();
        NSLog(@"[TCC-UI] CGPreflightScreenCaptureAccess returned: %d", hasPermission);

        if (!hasPermission) {
            // Call on background thread to avoid blocking UI (API blocks until user responds)
            NSLog(@"[TCC-UI] Calling CGRequestScreenCaptureAccess on background thread");
            dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                @autoreleasepool {
                    Boolean result = CGRequestScreenCaptureAccess();
                    NSLog(@"[TCC-UI] CGRequestScreenCaptureAccess returned: %d", result);
                }
            });
        } else {
            NSLog(@"[TCC-UI] Screen recording already granted, not requesting");
        }
    }

    // Also open System Settings so user can see MeshAgent was added to the list
    NSLog(@"[TCC-UI] Opening System Settings for Screen Recording");
    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

@end

// Window delegate to handle events
@interface TCCPermissionsWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) BOOL doNotRemindAgain;
@property (nonatomic, assign) BOOL windowClosed;
@property (nonatomic, strong) TCCButtonHandler *buttonHandler;
- (void)checkboxToggled:(NSButton*)sender;
@end

@implementation TCCPermissionsWindowDelegate

- (id)init {
    self = [super init];
    if (self) {
        _doNotRemindAgain = NO;
        _windowClosed = NO;
    }
    return self;
}

- (void)windowWillClose:(NSNotification *)notification {
    _windowClosed = YES;
    [self.buttonHandler stopPeriodicUpdates];
    [NSApp stopModal];
}

- (void)checkboxToggled:(NSButton*)sender {
    self.doNotRemindAgain = ([sender state] == NSControlStateValueOn);
}

@end

// Helper function to create label text
static NSTextField* createLabel(NSString* text, NSRect frame, BOOL bold) {
    NSTextField* label = [[NSTextField alloc] initWithFrame:frame];
    [label setStringValue:text];
    [label setBezeled:NO];
    [label setDrawsBackground:NO];
    [label setEditable:NO];
    [label setSelectable:NO];

    if (bold) {
        [label setFont:[NSFont boldSystemFontOfSize:13]];
    } else {
        [label setFont:[NSFont systemFontOfSize:12]];
        [label setTextColor:[NSColor secondaryLabelColor]];
    }

    return label;
}

// Helper function to create section with permission name, description, and button
static void createPermissionSection(NSView* contentView, NSString* title, NSString* description, CGFloat yPos, SEL action, id target, NSInteger buttonTag) {
    // Title label (bold)
    NSTextField* titleLabel = createLabel(title, NSMakeRect(40, yPos, 380, 20), YES);
    [contentView addSubview:titleLabel];

    // Description label (gray, wrapped)
    NSTextField* descLabel = createLabel(description, NSMakeRect(40, yPos - 35, 380, 32), NO);
    [descLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [[descLabel cell] setWraps:YES];
    [contentView addSubview:descLabel];

    // "Open Settings" button (shifted down 12px from text)
    NSButton* settingsButton = [[NSButton alloc] initWithFrame:NSMakeRect(440, yPos - 20, 140, 28)];
    [settingsButton setTitle:@"Open Settings"];
    [settingsButton setBezelStyle:NSBezelStyleRounded];
    [settingsButton setTarget:target];
    [settingsButton setAction:action];
    [settingsButton setTag:buttonTag];
    [contentView addSubview:settingsButton];
}

int show_tcc_permissions_window(void) {
    @autoreleasepool {
        // Create lock file to prevent multiple instances
        create_lock_file();

        // Create the application if it doesn't exist
        [NSApplication sharedApplication];

        // Set activation policy to allow window to show
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        // Create window
        NSRect frame = NSMakeRect(0, 0, 600, 355);
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:frame
            styleMask:(NSWindowStyleMaskTitled |
                      NSWindowStyleMaskClosable)
            backing:NSBackingStoreBuffered
            defer:NO];

        [window setTitle:@"MeshAgent - Security & Privacy Settings"];

        // Position in upper-right area to stay out of the way
        NSScreen* mainScreen = [NSScreen mainScreen];
        NSRect screenFrame = [mainScreen visibleFrame];
        NSRect windowFrame = [window frame];

        // Position 20 pixels from right edge and 20 pixels from top
        CGFloat xPos = screenFrame.origin.x + screenFrame.size.width - windowFrame.size.width - 20;
        CGFloat yPos = screenFrame.origin.y + screenFrame.size.height - windowFrame.size.height - 20;

        [window setFrameOrigin:NSMakePoint(xPos, yPos)];
        [window setLevel:NSFloatingWindowLevel];

        // Get content view
        NSView* contentView = [window contentView];

        // Create button handler
        TCCButtonHandler* buttonHandler = [[TCCButtonHandler alloc] initWithContentView:contentView];

        // Create delegate and link button handler
        TCCPermissionsWindowDelegate* delegate = [[TCCPermissionsWindowDelegate alloc] init];
        delegate.buttonHandler = buttonHandler;
        [window setDelegate:delegate];

        // Add icon using SF Symbols (macOS 11+)
        if (@available(macOS 11.0, *)) {
            NSImageView* iconView = [[NSImageView alloc] initWithFrame:NSMakeRect(40, 295, 40, 40)];
            NSImage* icon = [NSImage imageWithSystemSymbolName:@"checkmark.shield" accessibilityDescription:@"Security"];
            [iconView setImage:icon];
            [iconView setSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:32 weight:NSFontWeightRegular]];
            [contentView addSubview:iconView];
        }

        // Add header title
        NSTextField* titleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(90, 315, 490, 24)];
        [titleLabel setStringValue:@"Security & Privacy Settings"];
        [titleLabel setBezeled:NO];
        [titleLabel setDrawsBackground:NO];
        [titleLabel setEditable:NO];
        [titleLabel setSelectable:NO];
        [titleLabel setFont:[NSFont systemFontOfSize:16 weight:NSFontWeightBold]];
        [titleLabel setTextColor:[NSColor labelColor]];
        [contentView addSubview:titleLabel];

        // Add header description
        NSTextField* headerLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(90, 280, 490, 32)];
        [headerLabel setStringValue:@"Please grant MeshAgent all the required permissions for complete access functionality."];
        [headerLabel setBezeled:NO];
        [headerLabel setDrawsBackground:NO];
        [headerLabel setEditable:NO];
        [headerLabel setSelectable:NO];
        [headerLabel setFont:[NSFont systemFontOfSize:12]];
        [headerLabel setTextColor:[NSColor secondaryLabelColor]];
        [headerLabel setLineBreakMode:NSLineBreakByWordWrapping];
        [[headerLabel cell] setWraps:YES];
        [contentView addSubview:headerLabel];

        // Add permission sections
        createPermissionSection(
            contentView,
            @"Accessibility",
            @"Accessibility permission is required for this computer to be controlled during a remote session.",
            235,
            @selector(openAccessibilitySettings:),
            buttonHandler,
            BUTTON_TAG_ACCESSIBILITY
        );

        createPermissionSection(
            contentView,
            @"Full Disk Access",
            @"Full Disk Access permission is required for features such as file transfer and drag-and-drop.",
            165,
            @selector(openFullDiskAccessSettings:),
            buttonHandler,
            BUTTON_TAG_FDA
        );

        createPermissionSection(
            contentView,
            @"Screen & System Audio Recording",
            @"Screen & System Audio Recording permission is required for this computer's screen to be viewed during a remote session.",
            100,
            @selector(openScreenRecordingSettings:),
            buttonHandler,
            BUTTON_TAG_SCREEN_RECORDING
        );

        // Add "Do not remind me again" checkbox
        NSButton* checkbox = [[NSButton alloc] initWithFrame:NSMakeRect(20, 15, 250, 20)];
        [checkbox setButtonType:NSButtonTypeSwitch];
        [checkbox setTitle:@"Do not remind me again"];
        [checkbox setTarget:delegate];
        [checkbox setAction:@selector(checkboxToggled:)];
        [contentView addSubview:checkbox];

        // Add "Finish" button
        NSButton* finishButton = [[NSButton alloc] initWithFrame:NSMakeRect(490, 15, 90, 32)];
        [finishButton setTitle:@"Finish"];
        [finishButton setBezelStyle:NSBezelStyleRounded];
        [finishButton setKeyEquivalent:@"\r"]; // Enter key
        [finishButton setTarget:window];
        [finishButton setAction:@selector(close)];
        [contentView addSubview:finishButton];

        // Start real-time monitoring using notifications + light polling
        // Accessibility uses NSDistributedNotificationCenter (instant updates like Splashtop!)
        // Screen Recording uses 5-second polling (no notification available)
        [buttonHandler startPeriodicUpdates];

        // Show window and run modal
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp runModalForWindow:window];

        // Get result
        int result = delegate.doNotRemindAgain ? 1 : 0;

        // Cleanup
        [window close];
        remove_lock_file();

        return result;
    }
}

// Async wrapper implementation using ILibProcessPipe for spawning as user
// This spawns a child process with "-tccCheck" flag to show the UI
// Returns file descriptor for reading result from child, or -1 on error
int show_tcc_permissions_window_async(const char* exe_path, void* pipeManager, int uid) {
    // Check if TCC UI is already running
    if (is_tcc_ui_running()) {
        printf("[TCC-SPAWN] TCC UI already running - not spawning\n");
        return -1; // Don't spawn another instance
    }

    // Create pipe for IPC (child writes result, parent reads)
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        printf("[TCC-SPAWN] ERROR: Failed to create pipe: %s\n", strerror(errno));
        return -1;
    }

    // Set read end to non-blocking
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    printf("[TCC-SPAWN] Created pipe: read_fd=%d, write_fd=%d\n", pipefd[0], pipefd[1]);

    // Convert write-end fd to string for passing via argv
    char fd_str[16];
    snprintf(fd_str, sizeof(fd_str), "%d", pipefd[1]);

    // Build argv for child process
    char* const argv[] = {
        "meshagent",     // argv[0] (program name)
        "-tccCheck",     // argv[1] (flag)
        fd_str,          // argv[2] (pipe write-end fd)
        NULL             // argv terminator
    };

    printf("[TCC-SPAWN] ========================================\n");
    printf("[TCC-SPAWN] About to spawn -tccCheck:\n");
    printf("[TCC-SPAWN]   exe_path:    %s\n", exe_path);
    printf("[TCC-SPAWN]   pipeManager: %p\n", pipeManager);
    printf("[TCC-SPAWN]   UID:         %d\n", uid);
    printf("[TCC-SPAWN]   argv[0]:     %s\n", argv[0]);
    printf("[TCC-SPAWN]   argv[1]:     %s\n", argv[1]);
    printf("[TCC-SPAWN]   argv[2]:     %s\n", argv[2]);
    printf("[TCC-SPAWN] ========================================\n");

    if (pipeManager == NULL) {
        printf("[TCC-SPAWN] ERROR: pipeManager is NULL!\n");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    // Spawn child process as specified user (same approach as old -kvm0)
    // Note: Uses DEFAULT spawn type - ILibProcessPipe internally calls setuid() when uid != 0
    ILibProcessPipe_Process childProcess = ILibProcessPipe_Manager_SpawnProcessEx3(
        pipeManager,
        (char*)exe_path,
        argv,
        ILibProcessPipe_SpawnTypes_DEFAULT,  // Same as original -kvm0 code
        (void*)(intptr_t)uid,                // User ID to run as
        0                                    // No extra memory
    );

    if (childProcess == NULL) {
        printf("[TCC-SPAWN] ERROR: ILibProcessPipe_Manager_SpawnProcessEx3 returned NULL - spawn failed!\n");
        printf("[TCC-SPAWN] Check if executable exists and has correct permissions\n");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    printf("[TCC-SPAWN] SUCCESS: Spawn function returned non-NULL process handle\n");

    pid_t child_pid = ILibProcessPipe_Process_GetPID(childProcess);
    printf("[TCC-SPAWN] Spawned -tccCheck child process, PID=%d, UID=%d\n", child_pid, uid);

    // Parent process: close write end (parent only reads)
    close(pipefd[1]);

    printf("[TCC-SPAWN] Parent continuing with read_fd=%d for child pid=%d\n", pipefd[0], child_pid);

    // Return read-end file descriptor for parent to monitor
    return pipefd[0];
}
