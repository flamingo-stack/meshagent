/*
 * mac_display.h
 *
 * Display enumeration, selection and pointer mapping for the macOS KVM
 */

#ifndef MAC_DISPLAY_H_
#define MAC_DISPLAY_H_

#include <CoreGraphics/CoreGraphics.h>

#define MAC_DISPLAY_MAX 16

// One display as the console sees it: 1-based id = position in the list, bounds in points (global display coordinates)
typedef struct mac_display_t
{
	CGDirectDisplayID id;
	int x;
	int y;
	int w;
	int h;
} mac_display_t;

typedef struct mac_display_list_t
{
	int count;
	mac_display_t items[MAC_DISPLAY_MAX];
} mac_display_list_t;

// The display being captured: origin and size in points, capture size in pixels
typedef struct mac_display_view_t
{
	CGDirectDisplayID id;
	int x;
	int y;
	int ptW;
	int ptH;
	int pxW;
	int pxH;
} mac_display_view_t;

// Enumerates the online, non-mirrored displays (main first, then by display id); returns 1 when the list differs from the previous one
int mac_display_refresh(mac_display_list_t *list);
// 1-based position of a display in the list, 0 when absent
int mac_display_index(const mac_display_list_t *list, CGDirectDisplayID id);
// Display at a 1-based position, kCGNullDirectDisplay when out of range
CGDirectDisplayID mac_display_at(const mac_display_list_t *list, int index);
// Display to capture: the request when listed, else the current one when still listed, else the main display
CGDirectDisplayID mac_display_resolve(const mac_display_list_t *list, CGDirectDisplayID current, int requested);
// Fills the capture view of a listed display from its current mode; returns 0 when the display is not listed
int mac_display_view(const mac_display_list_t *list, CGDirectDisplayID id, mac_display_view_t *view);
// Maps a pointer position in capture pixels of the view to global display coordinates
void mac_display_map_pointer(const mac_display_view_t *view, int x, int y, double *gx, double *gy);
// MNG_KVM_GET_DISPLAYS reply (count, ids 1..N, selected id); returns the bytes written, 0 when the buffer is too small
int mac_display_encode_list(const mac_display_list_t *list, int selected, unsigned char *buffer, int bufferLen);
// MNG_KVM_DISPLAY_INFO (id, x, y, w, h per display, signed 16-bit coordinates); returns the bytes written, 0 when too small
int mac_display_encode_info(const mac_display_list_t *list, unsigned char *buffer, int bufferLen);

#endif /* MAC_DISPLAY_H_ */
