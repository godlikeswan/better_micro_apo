#pragma once

#include <windows.h>

namespace ui {

// Native window menu bar. The items are owner-drawn so the menu follows the
// app theme instead of always using the system light menu colors.
void CreateMenuBar(HWND window, bool showAllDevices);
void DestroyMenuBar(HWND window);
void RefreshMenuBar(HWND window);
void UpdateMenuShowAllDevices(HWND window, bool checked);
void UpdateMenuCommands(HWND window, bool busy, bool installEnabled,
                        bool uninstallEnabled);

// Menu owner-draw handlers. Return true when the item belongs to the menu bar.
bool MeasureMenuItem(HWND window, MEASUREITEMSTRUCT* item);
bool DrawMenuItem(HWND window, DRAWITEMSTRUCT* item);

} // namespace ui
