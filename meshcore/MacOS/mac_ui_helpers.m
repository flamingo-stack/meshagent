/*
Shared UI helper functions for macOS

Provides common UI element creation functions used across
TCC permissions and installation assistant windows.
*/

#import "mac_ui_helpers.h"

NSTextField* mesh_createLabel(NSString* text, NSRect frame, BOOL bold) {
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
