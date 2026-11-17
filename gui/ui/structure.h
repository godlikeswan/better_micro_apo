#pragma once

#include <windows.h>

// Builds the whole control tree and lays it out. The view modules only decide
// what controls exist; the widget factories in widgets.h decide how they look.

void CreateControls(HWND root);
void RefreshLatencyIcons();
void DestroyLatencyIcons();
void Layout(int width, int height);
void Relayout();
void RepaintAll();
void ResizeColumns();
