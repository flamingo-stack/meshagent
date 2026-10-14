/*
Copyright 2010 - 2018 Intel Corporation

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

#include "mac_display.h"
#include "../../meshdefines.h"
#include <arpa/inet.h>
#include <string.h>

static void mac_display_add(mac_display_list_t *list, CGDirectDisplayID id)
{
	CGRect bounds;
	mac_display_t *item;

	if (list->count >= MAC_DISPLAY_MAX) { return; }
	bounds = CGDisplayBounds(id);
	item = &list->items[list->count++];
	item->id = id;
	item->x = (int)bounds.origin.x;
	item->y = (int)bounds.origin.y;
	item->w = (int)bounds.size.width;
	item->h = (int)bounds.size.height;
}

int mac_display_refresh(mac_display_list_t *list)
{
	CGDirectDisplayID ids[MAC_DISPLAY_MAX];
	CGDirectDisplayID main = CGMainDisplayID();
	mac_display_list_t fresh;
	uint32_t count = 0;
	uint32_t i;
	int changed;

	memset(&fresh, 0, sizeof(fresh));
	// Online rather than active: a sleeping display is still captured, as it was before this list existed
	if (CGGetOnlineDisplayList(MAC_DISPLAY_MAX, ids, &count) != kCGErrorSuccess) { count = 0; }

	if (main != kCGNullDirectDisplay) { mac_display_add(&fresh, main); }
	for (i = 0; i < count; i++)
	{
		if (ids[i] == main || ids[i] == kCGNullDirectDisplay) { continue; }
		// A mirror shows the same picture as the display it mirrors, so a mirror set is listed once
		if (CGDisplayMirrorsDisplay(ids[i]) != kCGNullDirectDisplay) { continue; }
		mac_display_add(&fresh, ids[i]);
	}

	// Main first, the others by display id: an added display does not renumber the ones before it
	for (i = (main != kCGNullDirectDisplay) ? 2 : 1; i < (uint32_t)fresh.count; i++)
	{
		mac_display_t item = fresh.items[i];
		int j = (int)i - 1;
		while (j >= ((main != kCGNullDirectDisplay) ? 1 : 0) && fresh.items[j].id > item.id) { fresh.items[j + 1] = fresh.items[j]; j--; }
		fresh.items[j + 1] = item;
	}

	changed = (fresh.count != list->count) || (memcmp(fresh.items, list->items, sizeof(mac_display_t) * fresh.count) != 0);
	if (changed) { memcpy(list, &fresh, sizeof(fresh)); }
	return changed;
}

int mac_display_index(const mac_display_list_t *list, CGDirectDisplayID id)
{
	int i;
	for (i = 0; i < list->count; i++)
	{
		if (list->items[i].id == id) { return i + 1; }
	}
	return 0;
}

CGDirectDisplayID mac_display_at(const mac_display_list_t *list, int index)
{
	if (index < 1 || index > list->count) { return kCGNullDirectDisplay; }
	return list->items[index - 1].id;
}

CGDirectDisplayID mac_display_resolve(const mac_display_list_t *list, CGDirectDisplayID current, int requested)
{
	CGDirectDisplayID wanted = mac_display_at(list, requested);
	if (wanted != kCGNullDirectDisplay) { return wanted; }
	if (mac_display_index(list, current) != 0) { return current; }
	return list->count > 0 ? list->items[0].id : kCGNullDirectDisplay;
}

int mac_display_view(const mac_display_list_t *list, CGDirectDisplayID id, mac_display_view_t *view)
{
	int index = mac_display_index(list, id);
	const mac_display_t *item;
	CGDisplayModeRef mode;

	if (index == 0) { return 0; }
	item = &list->items[index - 1];
	view->id = id;
	view->x = item->x;
	view->y = item->y;
	view->ptW = item->w;
	view->ptH = item->h;
	view->pxW = item->w;
	view->pxH = item->h;

	// The capture has the mode's pixel size: twice the point size on a Retina display
	mode = CGDisplayCopyDisplayMode(id);
	if (mode != NULL)
	{
		if (CGDisplayModeGetPixelWidth(mode) > 0 && CGDisplayModeGetPixelHeight(mode) > 0)
		{
			view->pxW = (int)CGDisplayModeGetPixelWidth(mode);
			view->pxH = (int)CGDisplayModeGetPixelHeight(mode);
		}
		if (CGDisplayModeGetWidth(mode) > 0 && CGDisplayModeGetHeight(mode) > 0)
		{
			view->ptW = (int)CGDisplayModeGetWidth(mode);
			view->ptH = (int)CGDisplayModeGetHeight(mode);
		}
		CGDisplayModeRelease(mode);
	}
	return 1;
}

void mac_display_map_pointer(const mac_display_view_t *view, int x, int y, double *gx, double *gy)
{
	double sx = (view->pxW > 0 && view->ptW > 0) ? ((double)view->ptW / (double)view->pxW) : 1.0;
	double sy = (view->pxH > 0 && view->ptH > 0) ? ((double)view->ptH / (double)view->pxH) : 1.0;
	*gx = (double)view->x + (double)x * sx;
	*gy = (double)view->y + (double)y * sy;
}

static unsigned short mac_display_int16(int value)
{
	if (value < -32768) { value = -32768; }
	if (value > 32767) { value = 32767; }
	return htons((unsigned short)(short)value);
}

static unsigned short mac_display_uint16(int value)
{
	if (value < 0) { value = 0; }
	if (value > 65535) { value = 65535; }
	return htons((unsigned short)value);
}

int mac_display_encode_list(const mac_display_list_t *list, int selected, unsigned char *buffer, int bufferLen)
{
	unsigned short *out = (unsigned short*)buffer;
	int size = 8 + (2 * list->count);
	int i;

	if (bufferLen < size) { return 0; }
	out[0] = htons((unsigned short)MNG_KVM_GET_DISPLAYS);
	out[1] = htons((unsigned short)size);
	out[2] = htons((unsigned short)list->count);
	for (i = 0; i < list->count; i++) { out[3 + i] = htons((unsigned short)(i + 1)); }
	out[3 + list->count] = htons((unsigned short)selected);
	return size;
}

int mac_display_encode_info(const mac_display_list_t *list, unsigned char *buffer, int bufferLen)
{
	unsigned short *out = (unsigned short*)buffer;
	int size = 4 + (10 * list->count);
	int i;

	if (bufferLen < size) { return 0; }
	out[0] = htons((unsigned short)MNG_KVM_DISPLAY_INFO);
	out[1] = htons((unsigned short)size);
	for (i = 0; i < list->count; i++)
	{
		const mac_display_t *item = &list->items[i];
		out[2 + (5 * i)] = htons((unsigned short)(i + 1));
		out[3 + (5 * i)] = mac_display_int16(item->x);
		out[4 + (5 * i)] = mac_display_int16(item->y);
		out[5 + (5 * i)] = mac_display_uint16(item->w);
		out[6 + (5 * i)] = mac_display_uint16(item->h);
	}
	return size;
}
