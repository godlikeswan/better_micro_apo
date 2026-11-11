#pragma once

#include <windows.h>

namespace ui {

struct Palette {
    COLORREF background;
    COLORREF text;
    COLORREF disabledText;
    COLORREF frame;
    COLORREF error;
    COLORREF editBackground;
    COLORREF editText;
    COLORREF listBackground;
    COLORREF listText;
    COLORREF listHeaderBackground;

    // Owner-drawn controls (used in dark mode; light mode keeps the native
    // themed appearance).
    COLORREF controlBackground;
    COLORREF controlHot;
    COLORREF controlPressed;
    COLORREF controlBorder;
    COLORREF controlText;
    COLORREF accent;
    COLORREF accentText;
    COLORREF tabBackground;
    COLORREF tabText;
    COLORREF tabSelectedText;
    COLORREF checkBackground;
    COLORREF checkBorder;
    COLORREF checkMark;
};

struct Theme {
    Palette color;
    HBRUSH backgroundBrush;
    HBRUSH editBrush;
    HBRUSH listBrush;
    HBRUSH listHeaderBrush;
    bool dark;
};

// Detect the Windows "app mode" (Settings > Personalization > Colors).
bool SystemUsesDarkMode();

// Read the system theme, rebuild the palette/brushes, and enable dark mode for
// the common controls in this process.
void InitializeTheme();
void ShutdownTheme();

// Re-read the system theme and re-apply it to every existing window.
void RefreshTheme();

const Theme& GetTheme();

// Apply the native dark/light variant to a single control and to every
// descendant of root.
void ApplyNativeTheme(HWND window);
void ApplyNativeThemeToChildren(HWND root);
void ApplyTitleBarTheme(HWND window);

// Color handlers used by the window procedures. Return the background brush
// the control should use.
LRESULT HandleStaticColor(HDC dc, bool error);
LRESULT HandleButtonColor(HDC dc);
LRESULT HandleEditColor(HDC dc);
LRESULT HandleListBoxColor(HDC dc);

} // namespace ui
