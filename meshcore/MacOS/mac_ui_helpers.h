#ifndef MAC_UI_HELPERS_H
#define MAC_UI_HELPERS_H

#import <Cocoa/Cocoa.h>

/**
 * Shared UI helper functions for macOS
 *
 * Provides common UI element creation functions used across
 * TCC permissions and installation assistant windows.
 */

/**
 * Create a configured NSTextField label
 *
 * Creates a non-editable, non-selectable label with consistent styling.
 * Bold labels use 13pt bold system font, regular labels use 12pt system font
 * with secondary label color.
 *
 * @param text The text to display in the label
 * @param frame The frame (position and size) for the label
 * @param bold YES for bold text, NO for regular text with gray color
 * @return An autoreleased NSTextField configured as a label
 */
NSTextField* mesh_createLabel(NSString* text, NSRect frame, BOOL bold);

#endif // MAC_UI_HELPERS_H
