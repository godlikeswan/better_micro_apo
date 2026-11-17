#pragma once

#include <windows.h>

#include "../preferences.h"

namespace ui {

// Native window menu bar. The items are owner-drawn so the menu follows the
// app theme instead of always using the system light menu colors.
void CreateMenuBar(HWND window, gui::InterfaceMode mode);
void DestroyMenuBar(HWND window);
void RefreshMenuBar(HWND window);
void UpdateMenuMode(HWND window, gui::InterfaceMode mode);

// Menu owner-draw handlers. Return true when the item belongs to the menu bar.
bool MeasureMenuItem(HWND window, MEASUREITEMSTRUCT* item);
bool DrawMenuItem(HWND window, DRAWITEMSTRUCT* item);

} // namespace ui
