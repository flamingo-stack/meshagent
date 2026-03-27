// Test file to preview TCC permissions window without full MeshAgent build
//
// Compile and run:
//   cd /Users/denys/Documents/GitHub/MeshAgent
//   clang -framework Cocoa -framework ApplicationServices -framework CoreGraphics \
//         meshcore/MacOS/TCC_UI/test_window.m \
//         -lsqlite3 -o test_window && ./test_window

#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <dlfcn.h>
#include <sqlite3.h>

// ============ Minimal TCC detection stubs ============

typedef enum {
    TCC_PERMISSION_NOT_DETERMINED = 0,
    TCC_PERMISSION_DENIED = 1,
    TCC_PERMISSION_GRANTED_USER = 2,
    TCC_PERMISSION_GRANTED_MDM = 3
} TCC_PermissionStatus;

TCC_PermissionStatus check_accessibility_permission(void) {
    Boolean isTrusted = AXIsProcessTrusted();
    return isTrusted ? TCC_PERMISSION_GRANTED_USER : TCC_PERMISSION_DENIED;
}

TCC_PermissionStatus check_fda_permission(void) {
    // Try to open TCC.db - requires FDA
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("/Library/Application Support/com.apple.TCC/TCC.db", &db, SQLITE_OPEN_READONLY, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_close(db);
        return TCC_PERMISSION_GRANTED_USER;
    }
    return TCC_PERMISSION_DENIED;
}

TCC_PermissionStatus check_screen_recording_permission(void) {
    if (@available(macOS 10.15, *)) {
        if (CGPreflightScreenCaptureAccess()) {
            return TCC_PERMISSION_GRANTED_USER;
        }
    }
    return TCC_PERMISSION_DENIED;
}

// ============ Design constants ============

#define WINDOW_WIDTH 580
#define WINDOW_HEIGHT 380
#define WINDOW_CORNER_RADIUS 16
#define CARD_CORNER_RADIUS 12
#define BUTTON_CORNER_RADIUS 8

// Window background #212121, cards darker #161616
#define COLOR_WINDOW_BG [NSColor colorWithRed:0x21/255.0 green:0x21/255.0 blue:0x21/255.0 alpha:1.0]
#define COLOR_CARD_BG [NSColor colorWithRed:0x16/255.0 green:0x16/255.0 blue:0x16/255.0 alpha:1.0]
#define COLOR_SEPARATOR [NSColor colorWithRed:0x44/255.0 green:0x44/255.0 blue:0x44/255.0 alpha:1.0]
#define COLOR_ACCENT [NSColor colorWithRed:0xF5/255.0 green:0xA6/255.0 blue:0x23/255.0 alpha:1.0]
#define COLOR_TEXT_PRIMARY [NSColor whiteColor]
#define COLOR_TEXT_SECONDARY [NSColor colorWithRed:0x99/255.0 green:0x99/255.0 blue:0x99/255.0 alpha:1.0]
// Continue button uses a darker shade
#define COLOR_DISABLED_BG [NSColor colorWithRed:0x35/255.0 green:0x35/255.0 blue:0x35/255.0 alpha:1.0]
#define COLOR_DISABLED_TEXT [NSColor colorWithRed:0x70/255.0 green:0x70/255.0 blue:0x70/255.0 alpha:1.0]
#define COLOR_CLOSE_BUTTON [NSColor colorWithRed:0x88/255.0 green:0x88/255.0 blue:0x88/255.0 alpha:1.0]

#define BUTTON_TAG_ACCESSIBILITY 1
#define BUTTON_TAG_FDA 2
#define BUTTON_TAG_SCREEN_RECORDING 3

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

@end

// ============ Forward declarations ============
@class TCCPermissionsWindowDelegate;

@interface TCCPermissionsWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) BOOL doNotRemindAgain;
@property (nonatomic, assign) BOOL windowClosed;
@property (nonatomic, strong) id buttonHandler;
@property (nonatomic, strong) HoverButton *continueButton;
@property (nonatomic, weak) NSWindow *window;
- (void)updateContinueButtonState;
- (void)closeWindow:(id)sender;
@end

// ============ Button Handler ============
@interface TCCButtonHandler : NSObject
@property (nonatomic, weak) NSView *contentView;
@property (nonatomic, strong) NSTimer *updateTimer;
@property (nonatomic, assign) BOOL cancelled;
@property (nonatomic, weak) TCCPermissionsWindowDelegate *windowDelegate;
@end

@implementation TCCButtonHandler

- (instancetype)initWithContentView:(NSView*)view {
    self = [super init];
    if (self) {
        _contentView = view;
        _cancelled = NO;
    }
    return self;
}

- (void)replaceButtonWithSuccessIcon:(NSButton*)button {
    if ([[button title] isEqualToString:@"✓"]) return;

    [button setEnabled:NO];
    [button setBordered:NO];
    [button setWantsLayer:YES];
    button.layer.backgroundColor = [[NSColor clearColor] CGColor];

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

    [button setEnabled:YES];
    [button setBordered:NO];
    [button setWantsLayer:YES];
    button.layer.backgroundColor = [COLOR_ACCENT CGColor];
    button.layer.cornerRadius = BUTTON_CORNER_RADIUS;

    NSMutableAttributedString* attrTitle = [[NSMutableAttributedString alloc] initWithString:@"Setup"];
    [attrTitle addAttribute:NSForegroundColorAttributeName value:[NSColor blackColor] range:NSMakeRange(0, attrTitle.length)];
    [attrTitle addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:13 weight:NSFontWeightBold] range:NSMakeRange(0, attrTitle.length)];
    [button setAttributedTitle:attrTitle];
    [button setTitle:@"Setup"];
}

- (void)updatePermissionStatus {
    TCC_PermissionStatus accessibility = check_accessibility_permission();
    TCC_PermissionStatus fda = check_fda_permission();
    TCC_PermissionStatus screen_recording = check_screen_recording_permission();

    for (NSView *subview in [self.contentView subviews]) {
        if ([subview isKindOfClass:[NSButton class]]) {
            NSButton *button = (NSButton*)subview;
            NSInteger tag = [button tag];

            TCC_PermissionStatus status = TCC_PERMISSION_NOT_DETERMINED;
            if (tag == BUTTON_TAG_ACCESSIBILITY) status = accessibility;
            else if (tag == BUTTON_TAG_FDA) status = fda;
            else if (tag == BUTTON_TAG_SCREEN_RECORDING) status = screen_recording;
            else continue;

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
    [self updatePermissionStatus];

    __weak TCCButtonHandler *weakSelf = self;
    self.updateTimer = [NSTimer scheduledTimerWithTimeInterval:2.0 repeats:YES block:^(NSTimer *timer) {
        [weakSelf updatePermissionStatus];
    }];
}

- (void)stopPeriodicUpdates {
    [self.updateTimer invalidate];
    self.updateTimer = nil;
}

- (void)openAccessibilitySettings:(id)sender {
    NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
    AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);

    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

- (void)openFullDiskAccessSettings:(id)sender {
    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

- (void)openScreenRecordingSettings:(id)sender {
    if (@available(macOS 10.15, *)) {
        CGRequestScreenCaptureAccess();
    }
    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

@end

// ============ Window Delegate Implementation ============
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
    // Continue button always yellow #FFC008, only hover effect
}

- (void)closeWindow:(id)sender {
    _windowClosed = YES;
    [self.buttonHandler stopPeriodicUpdates];
    [self.window close];
    [NSApp stopModal];
}

@end

// ============ Helper to create permission row ============
static void createPermissionRow(NSView* container, NSString* title, NSString* description, CGFloat yPos, SEL action, id target, NSInteger buttonTag, BOOL addSeparator) {
    CGFloat containerWidth = container.frame.size.width;
    CGFloat rowPadding = 20;
    CGFloat rowHeight = 70;

    // Title – 16pt font, positioned from top of the cell
    NSTextField* titleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(rowPadding, yPos + rowHeight - 38, containerWidth - 140, 24)];
    [titleLabel setStringValue:title];
    [titleLabel setBezeled:NO];
    [titleLabel setDrawsBackground:NO];
    [titleLabel setEditable:NO];
    [titleLabel setSelectable:NO];
    [titleLabel setFont:[NSFont systemFontOfSize:16 weight:NSFontWeightBold]];
    [titleLabel setTextColor:COLOR_TEXT_PRIMARY];
    [container addSubview:titleLabel];

    // Description – 13pt font, more padding below title
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

    CGFloat rowHeightForButton = 70;
    HoverButton* setupButton = [[HoverButton alloc] initWithFrame:NSMakeRect(containerWidth - 100, yPos + (rowHeightForButton - 36) / 2, 80, 36)];
    [setupButton setTitle:@"Setup"];
    [setupButton setBezelStyle:NSBezelStyleRounded];
    [setupButton setBordered:NO];
    [setupButton setTarget:target];
    [setupButton setAction:action];
    [setupButton setTag:buttonTag];
    [setupButton setWantsLayer:YES];

    // Colors for hover/press effects
    setupButton.normalColor = COLOR_ACCENT;
    setupButton.hoverColor = [NSColor colorWithRed:0xFF/255.0 green:0xB8/255.0 blue:0x40/255.0 alpha:1.0];  // lighter
    setupButton.pressedColor = [NSColor colorWithRed:0xD9/255.0 green:0x8C/255.0 blue:0x1D/255.0 alpha:1.0]; // darker

    setupButton.layer.backgroundColor = [COLOR_ACCENT CGColor];
    setupButton.layer.cornerRadius = BUTTON_CORNER_RADIUS;

    NSMutableAttributedString* attrTitle = [[NSMutableAttributedString alloc] initWithString:@"Setup"];
    [attrTitle addAttribute:NSForegroundColorAttributeName value:[NSColor blackColor] range:NSMakeRange(0, attrTitle.length)];
    [attrTitle addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:13 weight:NSFontWeightBold] range:NSMakeRange(0, attrTitle.length)];
    [setupButton setAttributedTitle:attrTitle];
    [container addSubview:setupButton];

    if (addSeparator) {
        NSView* separator = [[NSView alloc] initWithFrame:NSMakeRect(0, yPos, containerWidth, 1)];
        [separator setWantsLayer:YES];
        separator.layer.backgroundColor = [COLOR_SEPARATOR CGColor];
        [container addSubview:separator];
    }
}

// ============ Main Window Function ============
int show_tcc_permissions_window(int show_reminder_checkbox) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

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

        [[window contentView] setWantsLayer:YES];
        [window contentView].layer.cornerRadius = WINDOW_CORNER_RADIUS;
        [window contentView].layer.masksToBounds = YES;

        [window center];
        [window setLevel:NSFloatingWindowLevel];

        NSView* contentView = [window contentView];
        [contentView setWantsLayer:YES];
        contentView.layer.backgroundColor = [COLOR_WINDOW_BG CGColor];

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

        // App icon (48×48)
        NSImageView* iconView = [[NSImageView alloc] initWithFrame:NSMakeRect(20, WINDOW_HEIGHT - 68, 48, 48)];

        // Для теста - загружаем из фиксированного пути
        NSImage* appIcon = [[NSImage alloc] initWithContentsOfFile:@"/Users/denys/Documents/GitHub/MeshAgent/build/resources/icon/openframe.icns"];

        // Fallback на bundle иконку
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
        [titleLabel setFont:[NSFont systemFontOfSize:17 weight:NSFontWeightBold]];
        [titleLabel setTextColor:COLOR_TEXT_PRIMARY];
        [contentView addSubview:titleLabel];

        // Subtitle
        NSTextField* subtitleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(80, WINDOW_HEIGHT - 68, 600, 20)];
        [subtitleLabel setStringValue:@"Let's get OpenFrame ready to help manage your systems securely."];
        [subtitleLabel setBezeled:NO];
        [subtitleLabel setDrawsBackground:NO];
        [subtitleLabel setEditable:NO];
        [subtitleLabel setFont:[NSFont systemFontOfSize:13]];
        [subtitleLabel setTextColor:COLOR_TEXT_SECONDARY];
        [contentView addSubview:subtitleLabel];

        // Cards container
        CGFloat cardsContainerY = 72;  // чуть выше
        CGFloat cardsContainerHeight = 210;  // 3 rows × 70pt
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

        createPermissionRow(cardsContainer, @"Accessibility",
            @"Required to remotely manage and monitor your devices.",
            rowHeight * 2, @selector(openAccessibilitySettings:), buttonHandler, BUTTON_TAG_ACCESSIBILITY, YES);

        createPermissionRow(cardsContainer, @"Full Disk Access",
            @"Enables secure file transfers and system maintenance tasks.",
            rowHeight * 1, @selector(openFullDiskAccessSettings:), buttonHandler, BUTTON_TAG_FDA, YES);

        createPermissionRow(cardsContainer, @"Screen & System Audio Recording",
            @"Allows technical support and diagnostics when needed.",
            0, @selector(openScreenRecordingSettings:), buttonHandler, BUTTON_TAG_SCREEN_RECORDING, NO);

        // Footer элементы по центру между низом карточек и низом окна
        // Низ карточек на cardsContainerY = 66, низ окна = 0
        // Центр = 66 / 2 = 33
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

        // Continue button - всегда жёлтый #FFC008
        NSColor* continueYellow = [NSColor colorWithRed:0xFF/255.0 green:0xC0/255.0 blue:0x08/255.0 alpha:1.0];

        HoverButton* continueButton = [[HoverButton alloc] initWithFrame:NSMakeRect(WINDOW_WIDTH - 120, footerCenterY - 18, 100, 36)];
        [continueButton setBezelStyle:NSBezelStyleRounded];
        [continueButton setBordered:NO];
        [continueButton setKeyEquivalent:@"\r"];
        [continueButton setTarget:delegate];
        [continueButton setAction:@selector(closeWindow:)];
        [continueButton setWantsLayer:YES];

        // Жёлтый цвет с hover эффектами
        continueButton.normalColor = continueYellow;
        continueButton.hoverColor = [NSColor colorWithRed:0xFF/255.0 green:0xCC/255.0 blue:0x33/255.0 alpha:1.0];  // Светлее
        continueButton.pressedColor = [NSColor colorWithRed:0xE6/255.0 green:0xAD/255.0 blue:0x00/255.0 alpha:1.0]; // Темнее

        continueButton.layer.backgroundColor = [continueYellow CGColor];
        continueButton.layer.cornerRadius = BUTTON_CORNER_RADIUS;

        NSMutableAttributedString* continueAttr = [[NSMutableAttributedString alloc] initWithString:@"Continue"];
        [continueAttr addAttribute:NSForegroundColorAttributeName value:[NSColor blackColor] range:NSMakeRange(0, continueAttr.length)];
        [continueAttr addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold] range:NSMakeRange(0, continueAttr.length)];
        [continueButton setAttributedTitle:continueAttr];
        [contentView addSubview:continueButton];

        delegate.continueButton = continueButton;

        [buttonHandler startPeriodicUpdates];

        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp runModalForWindow:window];

        return delegate.doNotRemindAgain ? 1 : 0;
    }
}

// ============ Main ============
int main(int argc, const char * argv[]) {
    @autoreleasepool {
        printf("Opening TCC permissions window...\n");
        printf("(Close the window to exit)\n\n");

        int result = show_tcc_permissions_window(1);

        printf("\nWindow closed. Result: %d\n", result);
        printf("  0 = User clicked Continue/Close\n");
        printf("  1 = User checked 'Do not remind me again'\n");

        return 0;
    }
}

