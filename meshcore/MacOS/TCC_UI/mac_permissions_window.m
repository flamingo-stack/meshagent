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
#include <execinfo.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <sqlite3.h>
#import "../mac_ui_helpers.h"

// Lock file to prevent multiple TCC UI processes
#define TCC_LOCK_FILE "/tmp/meshagent_tcccheck.lock"

// Button tags for identification
#define BUTTON_TAG_ACCESSIBILITY 1
#define BUTTON_TAG_FDA 2
#define BUTTON_TAG_SCREEN_RECORDING 3

// ============ Design constants - OpenFrame redesign ============
#define WINDOW_WIDTH 580
#define WINDOW_HEIGHT 380
#define WINDOW_CORNER_RADIUS 16
#define CARD_CORNER_RADIUS 12
#define BUTTON_CORNER_RADIUS 8

// Colors
#define COLOR_WINDOW_BG [NSColor colorWithRed:0x21/255.0 green:0x21/255.0 blue:0x21/255.0 alpha:1.0]
#define COLOR_CARD_BG [NSColor colorWithRed:0x16/255.0 green:0x16/255.0 blue:0x16/255.0 alpha:1.0]
#define COLOR_SEPARATOR [NSColor colorWithRed:0x44/255.0 green:0x44/255.0 blue:0x44/255.0 alpha:1.0]
#define COLOR_ACCENT [NSColor colorWithRed:0xF5/255.0 green:0xA6/255.0 blue:0x23/255.0 alpha:1.0]
#define COLOR_TEXT_PRIMARY [NSColor whiteColor]
#define COLOR_TEXT_SECONDARY [NSColor colorWithRed:0x99/255.0 green:0x99/255.0 blue:0x99/255.0 alpha:1.0]
#define COLOR_DISABLED_BG [NSColor colorWithRed:0x35/255.0 green:0x35/255.0 blue:0x35/255.0 alpha:1.0]
#define COLOR_DISABLED_TEXT [NSColor colorWithRed:0x70/255.0 green:0x70/255.0 blue:0x70/255.0 alpha:1.0]
#define COLOR_CLOSE_BUTTON [NSColor colorWithRed:0x88/255.0 green:0x88/255.0 blue:0x88/255.0 alpha:1.0]

// Helper functions for lock file management
static int is_process_running(pid_t pid) {
    return kill(pid, 0) == 0;
}

static int is_tcc_ui_running(void) {
    FILE* f = fopen(TCC_LOCK_FILE, "r");
    if (f == NULL) {
        return 0;
    }

    pid_t pid;
    if (fscanf(f, "%d", &pid) == 1) {
        fclose(f);
        if (is_process_running(pid)) {
            return 1;
        } else {
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

// ============ Custom Button with hover/press effects ============
@interface HoverButton : NSButton
@property (nonatomic, strong) NSColor *normalColor;
@property (nonatomic, strong) NSColor *hoverColor;
@property (nonatomic, strong) NSColor *pressedColor;
@property (nonatomic, strong) NSTrackingArea *trackingArea;
@end

@implementation HoverButton

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (self.trackingArea) {
        [self removeTrackingArea:self.trackingArea];
    }
    self.trackingArea = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                     options:(NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways)
                                                       owner:self
                                                    userInfo:nil];
    [self addTrackingArea:self.trackingArea];
}

- (void)mouseEntered:(NSEvent *)event {
    if (self.isEnabled && self.hoverColor) {
        self.layer.backgroundColor = [self.hoverColor CGColor];
    }
}

- (void)mouseExited:(NSEvent *)event {
    if (self.isEnabled && self.normalColor) {
        self.layer.backgroundColor = [self.normalColor CGColor];
    }
}

- (void)mouseDown:(NSEvent *)event {
    if (self.isEnabled && self.pressedColor) {
        self.layer.backgroundColor = [self.pressedColor CGColor];
    }
}

- (void)mouseUp:(NSEvent *)event {
    if (self.isEnabled && self.normalColor) {
        self.layer.backgroundColor = [self.normalColor CGColor];
    }
    // Manually trigger the action if mouse is still inside button
    if (self.isEnabled) {
        NSPoint locationInView = [self convertPoint:[event locationInWindow] fromView:nil];
        if (NSPointInRect(locationInView, self.bounds)) {
            [NSApp sendAction:self.action to:self.target from:self];
        }
    }
}

- (void)dealloc {
    if (self.trackingArea) {
        [self removeTrackingArea:self.trackingArea];
    }
    // ARC handles [super dealloc] automatically
}

@end

// Forward declaration
@class TCCPermissionsWindowDelegate;

@interface TCCPermissionsWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) BOOL doNotRemindAgain;
@property (nonatomic, assign) BOOL windowClosed;
@property (nonatomic, strong) id buttonHandler;
@property (nonatomic, strong) HoverButton *continueButton;
@property (nonatomic, weak) NSWindow *window;
- (void)checkboxToggled:(NSButton*)sender;
- (void)updateContinueButtonState;
- (void)closeWindow:(id)sender;
@end

// Button action handler class
@interface TCCButtonHandler : NSObject
@property (nonatomic, assign) NSView *contentView;
@property (nonatomic, strong) NSTimer *updateTimer;
@property (nonatomic, assign) BOOL cancelled;
@property (nonatomic, weak) TCCPermissionsWindowDelegate *windowDelegate;

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
        _cancelled = NO;
    }
    return self;
}

- (void)dealloc {
    self.cancelled = YES;
    [self stopPeriodicUpdates];
    // ARC handles [super dealloc] automatically
}

- (void)replaceButtonWithSuccessIcon:(NSButton*)button {
    if ([[button title] isEqualToString:@"✓"]) return;

    [button setEnabled:NO];
    [button setBordered:NO];
    [button setWantsLayer:YES];
    button.layer.backgroundColor = [[NSColor clearColor] CGColor];

    // Clear attributed title from "Setup" state
    NSMutableAttributedString* emptyAttr = [[NSMutableAttributedString alloc] initWithString:@""];
    [button setAttributedTitle:emptyAttr];

    // Green checkmark icon
    if (@available(macOS 11.0, *)) {
        NSImageSymbolConfiguration* config = [NSImageSymbolConfiguration configurationWithPointSize:24 weight:NSFontWeightMedium];
        NSImage* checkmark = [NSImage imageWithSystemSymbolName:@"checkmark.circle.fill" accessibilityDescription:@"Done"];
        checkmark = [checkmark imageWithSymbolConfiguration:config];
        [button setImage:checkmark];
        [button setImagePosition:NSImageOnly];
        [button setContentTintColor:[NSColor colorWithRed:0x7F/255.0 green:0xFF/255.0 blue:0x00/255.0 alpha:1.0]];
    }
    [button setTitle:@"✓"];
}

- (void)showButton:(NSButton*)button {
    if ([[button title] isEqualToString:@"Setup"] && [button isEnabled]) return;

    // Clear any existing image and reset tint color FIRST (from checkmark state)
    [button setImage:nil];
    [button setImagePosition:NSNoImage];
    if (@available(macOS 10.14, *)) {
        [button setContentTintColor:[NSColor blackColor]];
    }

    [button setEnabled:YES];
    [button setBordered:NO];
    [button setWantsLayer:YES];
    button.layer.backgroundColor = [COLOR_ACCENT CGColor];
    button.layer.cornerRadius = BUTTON_CORNER_RADIUS;

    // Set title first, then attributed title to ensure color is applied
    [button setTitle:@"Setup"];
    NSMutableAttributedString* attrTitle = [[NSMutableAttributedString alloc] initWithString:@"Setup"];
    [attrTitle addAttribute:NSForegroundColorAttributeName value:[NSColor blackColor] range:NSMakeRange(0, attrTitle.length)];
    [attrTitle addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:13 weight:NSFontWeightBold] range:NSMakeRange(0, attrTitle.length)];
    [button setAttributedTitle:attrTitle];

    // Force redraw
    [button setNeedsDisplay:YES];
}

- (void)updatePermissionStatus {
    if (self.cancelled || !self.contentView) {
        return;
    }

    TCC_PermissionStatus accessibility = check_accessibility_permission();
    TCC_PermissionStatus fda = check_fda_permission();
    TCC_PermissionStatus screen_recording = check_screen_recording_permission();

    NSArray *subviews = [self.contentView subviews];
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
                continue;
            }

            if (status == TCC_PERMISSION_GRANTED_USER || status == TCC_PERMISSION_GRANTED_MDM) {
                [self replaceButtonWithSuccessIcon:button];
            } else {
                [self showButton:button];
            }
        }
    }

    [self.windowDelegate updateContinueButtonState];
}

- (void)startPeriodicUpdates {
    // Initial state
    [self updatePermissionStatus];

    // Listen for the accessibility change notification so we can update the
    // button faster than waiting for the timer.  This notification only fires
    // once we have invoked AXIsProcessTrustedWithOptions, which is why the
    // call in openAccessibilitySettings is now synchronous again.
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:self
        selector:@selector(accessibilityPermissionChanged:)
        name:@"com.apple.accessibility.api"
        object:nil
        suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];

    // Also refresh when the application becomes active; users often leave the
    // helper window open while toggling a preference and then switch back.
    [[NSNotificationCenter defaultCenter]
        addObserver:self
        selector:@selector(applicationDidBecomeActive:)
        name:NSApplicationDidBecomeActiveNotification
        object:nil];

    // Timer for all permissions (we previously split this up, but polling every
    // couple of seconds is cheap and keeps the code simple).  The 2‑second
    // interval gives the accessibility change some breathing room.
    __weak typeof(self) weakSelf = self;
    self.updateTimer = [NSTimer timerWithTimeInterval:2.0
                                               repeats:YES
                                                 block:^(NSTimer *timer) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (strongSelf && !strongSelf.cancelled) {
            [strongSelf updatePermissionStatus];
        } else {
            [timer invalidate];
        }
    }];

    [[NSRunLoop currentRunLoop] addTimer:self.updateTimer forMode:NSRunLoopCommonModes];
}

- (void)accessibilityPermissionChanged:(NSNotification *)notification {
    // The notification may fire before the trusted state is fully updated.  We
    // do a delayed check on a background queue, then jump back to the main
    // thread to update only the accessibility button.  Use a weak/strong
    // reference pattern to avoid retain cycles under ARC.
    __weak typeof(self) weakSelf = self;

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.2 * NSEC_PER_SEC)),
                   dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        @autoreleasepool {
            __strong typeof(weakSelf) strongSelf = weakSelf;
            if (!strongSelf || strongSelf.cancelled) return;

            TCC_PermissionStatus accessibility = check_accessibility_permission();

            dispatch_async(dispatch_get_main_queue(), ^{
                if (!strongSelf.cancelled && strongSelf.contentView) {
                    NSArray *subviews = [strongSelf.contentView subviews];
                    for (NSView *subview in subviews) {
                        if ([subview isKindOfClass:[NSButton class]]) {
                            NSButton *button = (NSButton*)subview;
                            if ([button tag] == BUTTON_TAG_ACCESSIBILITY) {
                                if (accessibility == TCC_PERMISSION_GRANTED_USER ||
                                    accessibility == TCC_PERMISSION_GRANTED_MDM) {
                                    [strongSelf replaceButtonWithSuccessIcon:button];
                                } else {
                                    [strongSelf showButton:button];
                                }
                                break;
                            }
                        }
                    }
                }
            });
        }
    });
}

- (void)stopPeriodicUpdates {
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
    [[NSNotificationCenter defaultCenter] removeObserver:self];

    if (self.updateTimer) {
        [self.updateTimer invalidate];
        self.updateTimer = nil;
    }
}

- (void)openAccessibilitySettings:(id)sender {
    // Calling AXIsProcessTrustedWithOptions synchronously is important;
    // it not only prompts the user but also registers our process with the
    // accessibility system so notifications will be delivered when the
    // checkbox is toggled.  The async dispatch added during the redesign
    // sometimes executed after the user had already granted permission,
    // which meant we never received the update and the button never flipped.
    if (__builtin_available(macOS 10.9, *)) {
        const void *keys[] = { kAXTrustedCheckOptionPrompt };
        const void *values[] = { kCFBooleanTrue };
        CFDictionaryRef options = CFDictionaryCreate(
            kCFAllocatorDefault,
            keys,
            values,
            1,
            &kCFCopyStringDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);

        AXIsProcessTrustedWithOptions(options);
        CFRelease(options);
    }

    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

- (void)openFullDiskAccessSettings:(id)sender {
    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

typedef CGImageRef (*CGWindowListCreateImageFunc)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);

static BOOL isAppInScreenRecordingTCCList(void) {
    NSBundle* mainBundle = [NSBundle mainBundle];
    NSString* bundleId = [mainBundle bundleIdentifier];
    if (!bundleId) {
        return NO;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("/Library/Application Support/com.apple.TCC/TCC.db", &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        if (__builtin_available(macOS 10.15, *)) {
            return CGPreflightScreenCaptureAccess();
        }
        return NO;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT auth_value FROM access WHERE service='kTCCServiceScreenCapture' AND client=?";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return NO;
    }

    sqlite3_bind_text(stmt, 1, [bundleId UTF8String], -1, SQLITE_STATIC);

    BOOL existsInList = NO;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        existsInList = YES;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return existsInList;
}

// Called when the application becomes active (e.g. user switches back from
// System Preferences).  We schedule a short delay to allow the system to
// propagate any TCC changes before re‑checking.
- (void)applicationDidBecomeActive:(NSNotification *)notification {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [self updatePermissionStatus];
    });
}

- (void)openScreenRecordingSettings:(id)sender {
    BOOL alreadyInList = isAppInScreenRecordingTCCList();

    if (!alreadyInList) {
        BOOL triggered = NO;

        if (@available(macOS 12.3, *)) {
            Class SCShareableContentClass = NSClassFromString(@"SCShareableContent");
            if (SCShareableContentClass) {
                SEL selector = NSSelectorFromString(@"getShareableContentWithCompletionHandler:");
                if ([SCShareableContentClass respondsToSelector:selector]) {
                    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

                    void (^completionHandler)(id, NSError*) = ^(id content, NSError *error) {
                        dispatch_semaphore_signal(semaphore);
                    };

                    NSMethodSignature *sig = [SCShareableContentClass methodSignatureForSelector:selector];
                    NSInvocation *invocation = [NSInvocation invocationWithMethodSignature:sig];
                    [invocation setTarget:SCShareableContentClass];
                    [invocation setSelector:selector];
                    [invocation setArgument:&completionHandler atIndex:2];
                    [invocation invoke];

                    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
                    // ARC manages dispatch objects automatically (macOS 10.8+)

                    triggered = YES;
                }
            }
        }

        if (!triggered) {
            if (__builtin_available(macOS 10.15, *)) {
                CGWindowListCreateImageFunc createImageFunc = (CGWindowListCreateImageFunc)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");

                if (createImageFunc) {
                    CGImageRef screenshot = createImageFunc(
                        CGRectMake(0, 0, 1, 1),
                        kCGWindowListOptionOnScreenOnly,
                        kCGNullWindowID,
                        kCGWindowImageDefault
                    );

                    if (screenshot) {
                        CGImageRelease(screenshot);
                    }
                    triggered = YES;
                }
            }
        }

        if (!triggered) {
            if (__builtin_available(macOS 10.15, *)) {
                CGRequestScreenCaptureAccess();
            }
        }
    }

    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

@end

// Window delegate implementation
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

- (void)updateContinueButtonState {
    // Continue button is always yellow, no state change needed
}

- (void)closeWindow:(id)sender {
    _windowClosed = YES;
    [self.buttonHandler stopPeriodicUpdates];
    [self.window close];
    [NSApp stopModal];
}

@end

// Helper function to create permission row
static void createPermissionRow(NSView* container, NSString* title, NSString* description, CGFloat yPos, SEL action, id target, NSInteger buttonTag, BOOL addSeparator) {
    CGFloat containerWidth = container.frame.size.width;
    CGFloat rowPadding = 20;
    CGFloat rowHeight = 70;

    // Title label
    NSTextField* titleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(rowPadding, yPos + rowHeight - 38, containerWidth - 140, 24)];
    [titleLabel setStringValue:title];
    [titleLabel setBezeled:NO];
    [titleLabel setDrawsBackground:NO];
    [titleLabel setEditable:NO];
    [titleLabel setSelectable:NO];
    [titleLabel setFont:[NSFont systemFontOfSize:16 weight:NSFontWeightBold]];
    [titleLabel setTextColor:COLOR_TEXT_PRIMARY];
    [container addSubview:titleLabel];

    // Description label
    NSTextField* descLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(rowPadding, yPos + 2, containerWidth - 140, 32)];
    [descLabel setStringValue:description];
    [descLabel setBezeled:NO];
    [descLabel setDrawsBackground:NO];
    [descLabel setEditable:NO];
    [descLabel setSelectable:NO];
    [descLabel setFont:[NSFont systemFontOfSize:13]];
    [descLabel setTextColor:COLOR_TEXT_SECONDARY];
    [descLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [[descLabel cell] setWraps:YES];
    [container addSubview:descLabel];

    // Setup button with hover effects
    CGFloat rowHeightForButton = 70;
    HoverButton* setupButton = [[HoverButton alloc] initWithFrame:NSMakeRect(containerWidth - 100, yPos + (rowHeightForButton - 36) / 2, 80, 36)];
    [setupButton setTitle:@"Setup"];
    [setupButton setBezelStyle:NSBezelStyleRounded];
    [setupButton setBordered:NO];
    [setupButton setTarget:target];
    [setupButton setAction:action];
    [setupButton setTag:buttonTag];
    [setupButton setWantsLayer:YES];

    setupButton.normalColor = COLOR_ACCENT;
    setupButton.hoverColor = [NSColor colorWithRed:0xFF/255.0 green:0xB8/255.0 blue:0x40/255.0 alpha:1.0];
    setupButton.pressedColor = [NSColor colorWithRed:0xD9/255.0 green:0x8C/255.0 blue:0x1D/255.0 alpha:1.0];

    setupButton.layer.backgroundColor = [COLOR_ACCENT CGColor];
    setupButton.layer.cornerRadius = BUTTON_CORNER_RADIUS;

    NSMutableAttributedString* attrTitle = [[NSMutableAttributedString alloc] initWithString:@"Setup"];
    [attrTitle addAttribute:NSForegroundColorAttributeName value:[NSColor blackColor] range:NSMakeRange(0, attrTitle.length)];
    [attrTitle addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:13 weight:NSFontWeightBold] range:NSMakeRange(0, attrTitle.length)];
    [setupButton setAttributedTitle:attrTitle];
    [container addSubview:setupButton];

    // Separator
    if (addSeparator) {
        NSView* separator = [[NSView alloc] initWithFrame:NSMakeRect(0, yPos, containerWidth, 1)];
        [separator setWantsLayer:YES];
        separator.layer.backgroundColor = [COLOR_SEPARATOR CGColor];
        [container addSubview:separator];
    }
}

int show_tcc_permissions_window(int show_reminder_checkbox) {
    @autoreleasepool {
        create_lock_file();

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        // Create borderless window
        NSRect frame = NSMakeRect(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:frame
            styleMask:NSWindowStyleMaskBorderless
            backing:NSBackingStoreBuffered
            defer:NO];

        [window setTitle:@"OpenFrame – Setup Required"];
        [window setBackgroundColor:COLOR_WINDOW_BG];
        [window setOpaque:YES];
        [window setHasShadow:YES];
        [window setMovableByWindowBackground:YES];

        // Rounded corners
        if (@available(macOS 10.14, *)) {
            [[window contentView] setWantsLayer:YES];
            [window contentView].layer.cornerRadius = WINDOW_CORNER_RADIUS;
            [window contentView].layer.masksToBounds = YES;
        }

        // Position in upper-right
        NSScreen* mainScreen = [NSScreen mainScreen];
        NSRect screenFrame = [mainScreen visibleFrame];
        NSRect windowFrame = [window frame];
        CGFloat xPos = screenFrame.origin.x + screenFrame.size.width - windowFrame.size.width - 20;
        CGFloat yPos = screenFrame.origin.y + screenFrame.size.height - windowFrame.size.height - 20;
        [window setFrameOrigin:NSMakePoint(xPos, yPos)];
        [window setLevel:NSFloatingWindowLevel];

        NSView* contentView = [window contentView];
        [contentView setWantsLayer:YES];
        contentView.layer.backgroundColor = [COLOR_WINDOW_BG CGColor];

        // ============ HEADER ============

        // Create delegate early so we can use it for button targets
        TCCPermissionsWindowDelegate* delegate = [[TCCPermissionsWindowDelegate alloc] init];
        delegate.window = window;
        [window setDelegate:delegate];

        // Close button
        HoverButton* closeButton = [[HoverButton alloc] initWithFrame:NSMakeRect(WINDOW_WIDTH - 40, WINDOW_HEIGHT - 40, 24, 24)];
        [closeButton setTitle:@"×"];
        [closeButton setBordered:NO];
        [closeButton setTarget:delegate];
        [closeButton setAction:@selector(closeWindow:)];
        [closeButton setWantsLayer:YES];

        closeButton.normalColor = [NSColor clearColor];
        closeButton.hoverColor = [NSColor colorWithRed:0x50/255.0 green:0x50/255.0 blue:0x50/255.0 alpha:1.0];
        closeButton.pressedColor = [NSColor colorWithRed:0x40/255.0 green:0x40/255.0 blue:0x40/255.0 alpha:1.0];
        closeButton.layer.cornerRadius = 12;

        NSMutableAttributedString* closeAttr = [[NSMutableAttributedString alloc] initWithString:@"×"];
        [closeAttr addAttribute:NSForegroundColorAttributeName value:COLOR_CLOSE_BUTTON range:NSMakeRange(0, 1)];
        [closeAttr addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:18 weight:NSFontWeightLight] range:NSMakeRange(0, 1)];
        [closeButton setAttributedTitle:closeAttr];
        [contentView addSubview:closeButton];

        // App icon - load from bundle relative to executable
        NSImageView* iconView = [[NSImageView alloc] initWithFrame:NSMakeRect(20, WINDOW_HEIGHT - 68, 48, 48)];
        NSImage* appIcon = nil;

        // 1. Try main bundle (works when launched as .app)
        NSString* bundleIconPath = [[NSBundle mainBundle] pathForResource:@"openframe" ofType:@"icns"];
        if (bundleIconPath) {
            appIcon = [[NSImage alloc] initWithContentsOfFile:bundleIconPath];
        }

        // 2. Build path relative to executable: ../Resources/openframe.icns
        if (!appIcon) {
            NSString* execPath = [[NSBundle mainBundle] executablePath];
            if (execPath) {
                // execPath: .../MeshAgent.app/Contents/MacOS/meshagent
                // Go up to Contents, then into Resources
                NSString* resourcesPath = [[[[execPath stringByDeletingLastPathComponent]  // MacOS
                                              stringByDeletingLastPathComponent]            // Contents
                                             stringByAppendingPathComponent:@"Resources"]
                                            stringByAppendingPathComponent:@"openframe.icns"];
                if ([[NSFileManager defaultManager] fileExistsAtPath:resourcesPath]) {
                    appIcon = [[NSImage alloc] initWithContentsOfFile:resourcesPath];
                }
            }
        }

        // 3. Fallback to application icon
        if (!appIcon) {
            appIcon = [NSApp applicationIconImage];
        }

        if (appIcon) {
            [iconView setImage:appIcon];
        }
        [iconView setImageScaling:NSImageScaleProportionallyUpOrDown];
        [contentView addSubview:iconView];

        // Title
        NSTextField* titleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(80, WINDOW_HEIGHT - 45, 500, 24)];
        [titleLabel setStringValue:@"OpenFrame – Setup Required"];
        [titleLabel setBezeled:NO];
        [titleLabel setDrawsBackground:NO];
        [titleLabel setEditable:NO];
        [titleLabel setSelectable:NO];
        [titleLabel setFont:[NSFont systemFontOfSize:17 weight:NSFontWeightBold]];
        [titleLabel setTextColor:COLOR_TEXT_PRIMARY];
        [contentView addSubview:titleLabel];

        // Subtitle
        NSTextField* subtitleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(80, WINDOW_HEIGHT - 68, 600, 20)];
        [subtitleLabel setStringValue:@"Let's get OpenFrame ready to help manage your systems securely."];
        [subtitleLabel setBezeled:NO];
        [subtitleLabel setDrawsBackground:NO];
        [subtitleLabel setEditable:NO];
        [subtitleLabel setSelectable:NO];
        [subtitleLabel setFont:[NSFont systemFontOfSize:13]];
        [subtitleLabel setTextColor:COLOR_TEXT_SECONDARY];
        [contentView addSubview:subtitleLabel];

        // ============ CARDS CONTAINER ============

        CGFloat cardsContainerY = 72;
        CGFloat cardsContainerHeight = 210;
        CGFloat cardsContainerWidth = WINDOW_WIDTH - 40;

        NSView* cardsContainer = [[NSView alloc] initWithFrame:NSMakeRect(20, cardsContainerY, cardsContainerWidth, cardsContainerHeight)];
        [cardsContainer setWantsLayer:YES];
        cardsContainer.layer.backgroundColor = [COLOR_CARD_BG CGColor];
        cardsContainer.layer.cornerRadius = CARD_CORNER_RADIUS;
        [contentView addSubview:cardsContainer];

        // Create handler (delegate was created earlier for close button)
        TCCButtonHandler* buttonHandler = [[TCCButtonHandler alloc] initWithContentView:cardsContainer];
        buttonHandler.windowDelegate = delegate;
        delegate.buttonHandler = buttonHandler;

        CGFloat rowHeight = 70;

        createPermissionRow(
            cardsContainer,
            @"Accessibility",
            @"Required to remotely manage and monitor your devices.",
            rowHeight * 2,
            @selector(openAccessibilitySettings:),
            buttonHandler,
            BUTTON_TAG_ACCESSIBILITY,
            YES
        );

        createPermissionRow(
            cardsContainer,
            @"Full Disk Access",
            @"Enables secure file transfers and system maintenance tasks.",
            rowHeight * 1,
            @selector(openFullDiskAccessSettings:),
            buttonHandler,
            BUTTON_TAG_FDA,
            YES
        );

        createPermissionRow(
            cardsContainer,
            @"Screen & System Audio Recording",
            @"Allows technical support and diagnostics when needed.",
            0,
            @selector(openScreenRecordingSettings:),
            buttonHandler,
            BUTTON_TAG_SCREEN_RECORDING,
            NO
        );

        // ============ FOOTER ============

        CGFloat footerCenterY = cardsContainerY / 2;

        // Checkbox
        if (show_reminder_checkbox) {
            NSButton* checkbox = [[NSButton alloc] initWithFrame:NSMakeRect(20, footerCenterY - 10, 250, 20)];
            [checkbox setButtonType:NSButtonTypeSwitch];
            [checkbox setTarget:delegate];
            [checkbox setAction:@selector(checkboxToggled:)];

            NSMutableAttributedString* checkboxTitle = [[NSMutableAttributedString alloc] initWithString:@"Do not remind me again"];
            [checkboxTitle addAttribute:NSForegroundColorAttributeName value:COLOR_TEXT_PRIMARY range:NSMakeRange(0, checkboxTitle.length)];
            [checkboxTitle addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:13] range:NSMakeRange(0, checkboxTitle.length)];
            [checkbox setAttributedTitle:checkboxTitle];
            [contentView addSubview:checkbox];
        }

        // Continue button - always yellow #FFC008
        NSColor* continueYellow = [NSColor colorWithRed:0xFF/255.0 green:0xC0/255.0 blue:0x08/255.0 alpha:1.0];

        HoverButton* continueButton = [[HoverButton alloc] initWithFrame:NSMakeRect(WINDOW_WIDTH - 120, footerCenterY - 18, 100, 36)];
        [continueButton setBezelStyle:NSBezelStyleRounded];
        [continueButton setBordered:NO];
        [continueButton setKeyEquivalent:@"\r"];
        [continueButton setTarget:delegate];
        [continueButton setAction:@selector(closeWindow:)];
        [continueButton setWantsLayer:YES];

        continueButton.normalColor = continueYellow;
        continueButton.hoverColor = [NSColor colorWithRed:0xFF/255.0 green:0xCC/255.0 blue:0x33/255.0 alpha:1.0];
        continueButton.pressedColor = [NSColor colorWithRed:0xE6/255.0 green:0xAD/255.0 blue:0x00/255.0 alpha:1.0];

        continueButton.layer.backgroundColor = [continueYellow CGColor];
        continueButton.layer.cornerRadius = BUTTON_CORNER_RADIUS;

        NSMutableAttributedString* continueAttr = [[NSMutableAttributedString alloc] initWithString:@"Continue"];
        [continueAttr addAttribute:NSForegroundColorAttributeName value:[NSColor blackColor] range:NSMakeRange(0, continueAttr.length)];
        [continueAttr addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold] range:NSMakeRange(0, continueAttr.length)];
        [continueButton setAttributedTitle:continueAttr];
        [contentView addSubview:continueButton];

        delegate.continueButton = continueButton;

        // Start monitoring
        [buttonHandler startPeriodicUpdates];

        // Show window
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp runModalForWindow:window];

        int result = delegate.doNotRemindAgain ? 1 : 0;

        [window close];
        remove_lock_file();

        return result;
    }
}

// Async wrapper for spawning as user
int show_tcc_permissions_window_async(const char* exe_path, void* pipeManager, int uid) {
    char*** argvPtr = _NSGetArgv();
    int* argcPtr = _NSGetArgc();

    if (argvPtr && argcPtr) {
        char** argv = *argvPtr;
        int argc = *argcPtr;

        const char* forbidden_flags[] = {
            "-upgrade", "-install", "-fullinstall",
            "-uninstall", "-fulluninstall", "-update"
        };

        for (int i = 0; i < argc; i++) {
            for (int j = 0; j < 6; j++) {
                if (strcmp(argv[i], forbidden_flags[j]) == 0) {
                    return -1;
                }
            }
        }
    }

    if (is_tcc_ui_running()) {
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }

    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    char fd_str[16];
    snprintf(fd_str, sizeof(fd_str), "%d", pipefd[1]);

    char* const argv[] = {
        (char*)exe_path,
        "-tccCheck",
        fd_str,
        NULL
    };

    if (pipeManager == NULL) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    ILibProcessPipe_Process childProcess = ILibProcessPipe_Manager_SpawnProcessEx3(
        pipeManager,
        (char*)exe_path,
        argv,
        ILibProcessPipe_SpawnTypes_DEFAULT,
        (void*)(intptr_t)uid,
        0
    );

    if (childProcess == NULL) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    close(pipefd[1]);

    return pipefd[0];
}
