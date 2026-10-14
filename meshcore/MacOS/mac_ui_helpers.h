/*
Copyright 2024 Intel Corporation

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

